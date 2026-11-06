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
};

struct spec {
  int id;     // speculation id, 0 if this slot is free
  int uid;    // user who began it; only they (or admin) may join
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
  if (s == 0 || s->ending || (p->uid != 0 && p->uid != s->uid)) {
    release(&spec_lock);
    return -1;
  }
  acquire(&p->lock);
  p->spec = id;
  release(&p->lock);
  release(&spec_lock);
  return 0;
}

// Make one held record visible.  The caller is no longer speculative,
// so the write goes straight through instead of being held again.
static void
release_held(struct spec *s, struct held *h)
{
  uint64 src = (uint64)(s->buf + h->off);

  if (h->pipe)
    pipewrite(h->pipe, 0, src, h->n);
  else
    devsw[CONSOLE].write(0, src, h->n);
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
  struct spec *s;

  acquire(&spec_lock);
  s = lookup(id);
  if (s == 0 || s->ending) {
    release(&spec_lock);
    return -1;
  }
  s->ending = 1;
  release(&spec_lock);

  proc_leave_spec(id);

  for (int i = 0; i < s->nheld; i++) {
    if (commit)
      release_held(s, &s->held[i]);
    if (s->held[i].pipe)
      pipe_unhold(s->held[i].pipe);
  }

  acquire(&spec_lock);
  s->id = 0;
  release(&spec_lock);
  return 0;
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
    if (pi)
      pipe_hold(pi);
  }
  if (either_copyin(s->buf + s->used, user_src, src, n) < 0)
    goto fail;
  h->n += n;
  s->used += n;
  release(&spec_lock);
  return n;

fail:
  release(&spec_lock);
  return -1;
}
