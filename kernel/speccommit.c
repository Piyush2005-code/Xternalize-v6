// Committing a speculation's file changes, and crash injection.
//
// While a process runs inside a speculation, the shadow table (shadow.c)
// keeps its block writes in memory.  xcommit (spec_end in spec.c) calls
// spec_commit_files() to put them on disk, all in ONE log transaction:
//
//   1. the real blocks are read with bread_raw(), and the buffers are not
//      private ones, so log_write() stages them in the real log;
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
// The shadow table itself is in shadow.c.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "defs.h"

// One xcommit is one transaction, so the shadow table must fit in it.
_Static_assert(MAXSHADOWBLOCKS <= SPECBLOCKS,
               "MAXSHADOWBLOCKS must fit in one log transaction");

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
// Returns 0, -1 if the speculation holds more blocks than one transaction
// can, or -2 if an outside write changed one of its shadowed blocks.
int
spec_commit_files(int id)
{
  struct shadowent {
    uint dev;
    uint blockno;
    uint version;
    char *data;
    struct buf *bp;
  } ents[MAXSHADOWBLOCKS];
  int n = shadow_count(id);

  // The table was just read under its lock, so any refused write of a
  // member (which marks the speculation failed under that lock) is visible.
  if (n > SPECBLOCKS || spec_failed(id))
    return -1;

  crash_armed = 1;
  if (n > 0) {
    // Snapshot the table and sort by block address. Every commit takes locks
    // in this order, preventing two overlapping commits from deadlocking.
    for (int i = 0; i < n; i++) {
      if (shadow_get(id, i, &ents[i].dev, &ents[i].blockno, &ents[i].data,
                     &ents[i].version) < 0)
        panic("spec_commit_files: shadow vanished");
      ents[i].bp = 0;
    }
    for (int i = 1; i < n; i++) {
      struct shadowent key = ents[i];
      int j = i - 1;
      while (j >= 0 &&
             (ents[j].dev > key.dev ||
              (ents[j].dev == key.dev && ents[j].blockno > key.blockno))) {
        ents[j + 1] = ents[j];
        j--;
      }
      ents[j + 1] = key;
    }

    begin_op_n(n);
    int conflict = 0;
    for (int i = 0; i < n; i++) {
      ents[i].bp = bread_raw(ents[i].dev, ents[i].blockno);
      if (blockver_version(ents[i].dev, ents[i].blockno) !=
          ents[i].version)
        conflict = 1;
    }
    if (conflict) {
      for (int i = 0; i < n; i++)
        brelse(ents[i].bp);
      end_op_n(n); // no blocks were staged, so this transaction is empty
      crash_armed = 0;
      return -2;
    }

    for (int i = 0; i < n; i++) {
      memmove(ents[i].bp->data, ents[i].data, BSIZE);
      if (log_write(ents[i].bp) < 0)
        panic("spec_commit_files: real log refused shadow");
      crash_point(); // crashat(): after staging a shadow block
    }
    for (int i = 0; i < n; i++)
      brelse(ents[i].bp);
    end_op_n(n); // the transaction commits here
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
