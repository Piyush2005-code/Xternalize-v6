// Externalization barrier for speculative processes.
//
// A speculation is a group of processes whose effects on the outside
// world are held back: nothing they print is seen until the speculation
// commits, and everything is thrown away if it aborts.  This file keeps
// the table of open speculations and, for each one, the output its
// processes produced that nobody outside has seen yet.
//
// Held output is kept in arrival order in s->buf.  Each record in
// s->held says where one run of bytes was going (the console, or a pipe
// whose reader is outside the speculation), where it starts and how
// long it is.  Consecutive writes to the same place extend the last
// record, so a program that prints one character at a time still uses
// one record.
//
// Running out of room makes the write fail with -1.  Falling back to
// writing for real would externalize, which is what the barrier exists
// to prevent.
//
// Locking: spec_lock protects every field of every slot.  Once a slot is
// ending, no new output is added to it, so spec_end() can release the
// held records without holding the lock.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "defs.h"

struct held {
  struct pipe *pipe; // where the bytes were going; 0 means the console
  int off;           // where the bytes start in buf
  int n;             // how many bytes
  int done;          // bytes already pulled by an inside reader
};

struct spec {
  int id;     // speculation id, 0 if this slot is free
  int uid;    // user who began it; only they (or admin) may join
  int owner;  // login session that owns it; reclaimed when it exits
  int ending; // commit or abort in progress: hold nothing more
  int nheld;  // records in use
  struct held held[NHELD];
  int used; // bytes of buf in use
  char buf[HELDBYTES];
};

static struct spinlock spec_lock;
static struct spec specs[NSPEC];
static int nextid = 1;

void
spec_init(void)
{
  initlock(&spec_lock, "spec");
}

// Find the open speculation with this id.  Caller holds spec_lock.
static struct spec *
lookup(int id)
{
  struct spec *s;

  for (s = specs; s < &specs[NSPEC]; s++)
    if (id != 0 && s->id == id)
      return s;
  return 0;
}

// Drop a slot's held output and free it, without releasing anything.
// Caller has set s->ending under spec_lock and does not hold the lock.
static void
discard_slot(struct spec *s, int id)
{
  proc_leave_spec(id);
  spec_discard_files(id);
  for (int i = 0; i < s->nheld; i++)
    if (s->held[i].pipe)
      pipe_unhold(s->held[i].pipe);
  acquire(&spec_lock);
  s->id = 0;
  release(&spec_lock);
}

// Abort every open speculation whose owning session is no longer alive
// (its shell exited or was killed without committing).  Lets a crashed
// session's slots be recovered on demand, so a new xbegin can proceed.
static void
reclaim_dead(void)
{
  for (struct spec *s = specs; s < &specs[NSPEC]; s++) {
    int id, owner;

    acquire(&spec_lock);
    id = s->id;
    owner = s->owner;
    if (id == 0 || s->ending)
      id = 0;
    release(&spec_lock);
    if (id == 0 || owner == 0 || session_alive(owner))
      continue;

    acquire(&spec_lock);
    if (s->id == id && !s->ending)
      s->ending = 1;
    else
      id = 0;
    release(&spec_lock);
    if (id)
      discard_slot(s, id);
  }
}

// xbegin(): open a new speculation and put the calling process in it.
// Returns the speculation id, or -1 if the caller is already speculative
// (nesting is not supported) or all NSPEC slots are in use.
int
spec_begin(void)
{
  struct proc *p = myproc();
  struct spec *s;

  if (p->spec != 0)
    return -1;

  reclaim_dead(); // free any slot whose session has gone away

  acquire(&spec_lock);
  for (s = specs; s < &specs[NSPEC]; s++)
    if (s->id == 0)
      break;
  if (s == &specs[NSPEC]) {
    release(&spec_lock);
    return -1;
  }
  s->id = nextid++;
  s->uid = p->uid;
  s->owner = p->sid;
  s->ending = 0;
  s->nheld = 0;
  s->used = 0;
  acquire(&p->lock);
  p->spec = s->id;
  release(&p->lock);
  release(&spec_lock);
  return s->id;
}

// xjoin(id): put the calling process into an open speculation, so that
// a process that did not create it (a shell running one command, say)
// can work inside it.  Only the user who began it, or admin, may join.
int
spec_join(int id)
{
  struct proc *p = myproc();
  struct spec *s;

  if (p->spec != 0)
    return -1;

  acquire(&spec_lock);
  s = lookup(id);
  // Must belong to the caller's own login session, and the caller must
  // be the user who began it (or admin).
  if (s == 0 || s->ending || s->owner != p->sid ||
      (p->uid != 0 && p->uid != s->uid)) {
    release(&spec_lock);
    return -1;
  }
  acquire(&p->lock);
  p->spec = id;
  release(&p->lock);
  release(&spec_lock);
  return 0;
}

// Abort every open speculation owned by login session sid.  Called from
// kexit when the session's leader exits, so a speculation left open at
// logout does not hold its slot.
void
spec_reclaim_session(int sid)
{
  for (struct spec *s = specs; s < &specs[NSPEC]; s++) {
    int id = 0;

    acquire(&spec_lock);
    if (s->id != 0 && !s->ending && s->owner == sid) {
      id = s->id;
      s->ending = 1;
    }
    release(&spec_lock);
    if (id)
      discard_slot(s, id);
  }
}

// Make one held record visible.  The caller is no longer speculative,
// so the write goes straight through instead of being held again.  Only
// the part an inside reader has not already pulled (h->done..h->n) is
// released, so nothing is delivered twice.
static void
release_held(struct spec *s, struct held *h)
{
  uint64 src = (uint64)(s->buf + h->off + h->done);
  int n = h->n - h->done;

  if (n <= 0)
    return;
  if (h->pipe)
    pipewrite(h->pipe, 0, src, n);
  else
    devsw[CONSOLE].write(0, src, n);
}

// xcommit() (commit = 1) and xabort() (commit = 0) for the calling
// process's speculation.  Every member leaves the speculation; on commit
// the held output is then released in the order it was produced, on
// abort it is dropped.  Returns -1 if the caller is not speculative.
int
spec_end(int commit)
{
  struct proc *p = myproc();
  int id = p->spec;
  int commit_requested = commit;
  struct spec *s;

  acquire(&spec_lock);
  s = lookup(id);
  if (s == 0 || s->ending) {
    release(&spec_lock);
    return -1;
  }
  s->ending = 1;
  release(&spec_lock);

  // Commit order (docs/speculation-commit.md): the file changes go to disk
  // in one log transaction first, and only after end_op() has returned do
  // the members leave and the held output come out.  A crash before that
  // leaves no output behind for changes that did not happen.  If the
  // changes cannot be applied the speculation is dropped instead.
  if (commit && spec_commit_files(id) < 0)
    commit = 0;

  proc_leave_spec(id);
  spec_discard_files(id); // the shadows are on disk now (or dropped)

  for (int i = 0; i < s->nheld; i++) {
    if (commit)
      release_held(s, &s->held[i]);
    if (s->held[i].pipe)
      pipe_unhold(s->held[i].pipe);
  }

  acquire(&spec_lock);
  s->id = 0;
  release(&spec_lock);
  return commit == commit_requested ? 0 : -1;
}

// Hold n bytes that the calling (speculative) process is writing to pipe
// pi, or to the console if pi is 0, instead of writing them.  The bytes
// come from user space if user_src is set.  Returns n, or -1 if the
// speculation has no room left.
int
spec_hold(struct pipe *pi, int user_src, uint64 src, int n)
{
  struct proc *p = myproc();
  struct spec *s;
  struct held *h;

  acquire(&spec_lock);
  s = lookup(p->spec);
  if (s == 0 || s->ending || n > HELDBYTES - s->used)
    goto fail;

  h = s->nheld > 0 ? &s->held[s->nheld - 1] : 0;
  if (h == 0 || h->pipe != pi) {
    if (s->nheld == NHELD)
      goto fail;
    h = &s->held[s->nheld++];
    h->pipe = pi;
    h->off = s->used;
    h->n = 0;
    h->done = 0;
    if (pi)
      pipe_hold(pi);
  }
  if (either_copyin(s->buf + s->used, user_src, src, n) < 0)
    goto fail;
  h->n += n;
  s->used += n;
  release(&spec_lock);

  // Wake a reader that is inside this speculation so it can pull the
  // bytes (see spec_pull); a reader outside it just goes back to sleep.
  if (pi)
    wakeup(pipe_readchan(pi));
  return n;

fail:
  release(&spec_lock);
  return -1;
}

// An inside reader of pipe pi pulls the bytes its own speculation is
// holding for pi, in order, straight from the held buffer.  This is how
// a pipe whose reader turns out to be inside the speculation is not
// deadlocked: the write is held, and the inside reader retrieves it
// here instead of waiting for a commit that would never come.  A reader
// outside the speculation (p->spec does not match) pulls nothing and so
// waits for the commit, which is correct.  Returns bytes delivered (0 if
// none to pull), or -1 on a copyout error.
int
spec_pull(struct pipe *pi, uint64 addr, int n)
{
  struct proc *p = myproc();
  struct spec *s;
  struct held *h = 0;
  struct pipe *finished = 0;
  int i;

  acquire(&spec_lock);
  s = lookup(p->spec);
  if (s == 0 || s->ending) {
    release(&spec_lock);
    return 0;
  }
  for (int k = 0; k < s->nheld; k++)
    if (s->held[k].pipe == pi && s->held[k].done < s->held[k].n) {
      h = &s->held[k];
      break;
    }
  if (h == 0) {
    release(&spec_lock);
    return 0;
  }
  for (i = 0; i < n && h->done < h->n; i++) {
    char ch = s->buf[h->off + h->done];
    if (copyout(p->pagetable, p->sz, addr + i, &ch, 1) == -1) {
      if (i == 0)
        i = -1;
      break;
    }
    h->done++;
  }
  if (h->done == h->n && h->pipe) {
    finished = h->pipe;
    h->pipe = 0; // fully pulled: spec_end must not release it again
  }
  release(&spec_lock);

  if (finished)
    pipe_unhold(finished);
  return i;
}
