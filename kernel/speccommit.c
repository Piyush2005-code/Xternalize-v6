// Committing a speculation's file changes, and crash injection.
//
// While a process runs inside a speculation, the shadow table (shadow.c)
// keeps its block writes in memory.  xcommit (spec_end in spec.c) calls
// spec_commit_files() to put them on disk, all in ONE log transaction:
//
//   1. the caller becomes non-speculative for the loop, so bread() and
//      log_write() reach the real blocks, not the shadows;
//   2. begin_op_n(n) reserves room for all n shadow blocks;
//   3. each shadow is copied over its real block and log_write()n;
//   4. end_op_n(n) commits the transaction (write_head is the commit
//      point: before it the files are old, after it they are new).
//
// spec_end releases the held console and pipe output only after this
// function returns, i.e. after end_op() - so a crash during the commit can
// never have printed something that then did not happen.
//
// crashat(n) (sys_crashat) makes the n-th crash point panic, to test that:
//   the staging of each shadow block (n = 1..k), the point just before
//   write_head, the point just after it, one point per installed block,
//   and the point just after end_op() returns (before any output).
//
// The shadow table itself is not here.  The weak functions below are
// TEMPORARY stand-ins until Heril's shadow table is merged; his real
// definitions replace them at link time.  They make every speculation
// look like it has no shadows, so file changes are not held.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "defs.h"

struct shadow;

// One xcommit is one transaction, so the shadow table must fit in it.
#ifdef MAXSHADOWBLOCKS
_Static_assert(MAXSHADOWBLOCKS <= SPECBLOCKS,
               "MAXSHADOWBLOCKS must fit in one log transaction");
#endif

// TEMP until Heril's PR merges: his shadow.c defines these, same names and
// signatures (shadow_get(), the iterator, is real now and has no stub).
struct shadow *shadow_find(int, uint, uint) __attribute__((weak));
struct shadow *shadow_alloc(int, uint, uint, struct buf *)
  __attribute__((weak));
void shadow_free_all(int) __attribute__((weak));
int shadow_count(int) __attribute__((weak));

struct shadow *
shadow_find(int spec, uint dev, uint blockno)
{
  return 0;
}

struct shadow *
shadow_alloc(int spec, uint dev, uint blockno, struct buf *b)
{
  return 0;
}

void
shadow_free_all(int spec)
{
}

int
shadow_count(int spec)
{
  return 0;
}

static int crash_left;  // panic at this crash point; 0 = off
static int crash_armed; // inside spec_commit_files

// crashat(n): the n-th crash point of the next speculative commit panics.
// n == 0 turns it off.
void
crash_set(int n)
{
  crash_left = n > 0 ? n : 0;
}

void
crash_point(void)
{
  if (crash_armed && crash_left > 0 && --crash_left == 0)
    panic("crashat: injected crash");
}

// Write every shadow of speculation id to disk in one transaction.
// Returns 0, or -1 if the speculation holds more blocks than one
// transaction can (nothing is written then).
int
spec_commit_files(int id)
{
  struct proc *p = myproc();
  int n = shadow_count(id);
  int saved;

  if (n > SPECBLOCKS)
    return -1;

  crash_armed = 1;
  if (n > 0) {
    // Read and write the real blocks, not our own shadows.
    acquire(&p->lock);
    saved = p->spec;
    p->spec = 0;
    release(&p->lock);

    begin_op_n(n);
    for (int i = 0; i < n; i++) {
      uint dev, blockno;
      char *data;
      struct buf *bp;

      if (shadow_get(id, i, &dev, &blockno, &data) < 0)
        panic("spec_commit_files: shadow vanished");
      bp = bread(dev, blockno);
      memmove(bp->data, data, BSIZE);
      log_write(bp);
      brelse(bp);
      crash_point(); // crashat(): after staging a shadow block
    }
    end_op_n(n); // the transaction commits here

    acquire(&p->lock);
    p->spec = saved;
    release(&p->lock);
  }
  crash_point(); // crashat(): files committed, output not yet released
  crash_armed = 0;
  return 0;
}

// Drop speculation id's shadows (xabort, and after a commit has applied
// them) and turn crash injection off.
void
spec_discard_files(int id)
{
  shadow_free_all(id);
  crash_armed = 0;
  crash_left = 0;
}
