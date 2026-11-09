// Block version counter for speculative commit conflict detection.
//
// Every disk block carries a version that is bumped whenever the block is
// written for real (outside any speculation).  When a speculation shadows a
// block it records the block's current version; at xcommit time each shadowed
// block's version is re-checked.  If an outsider changed a shadowed block
// after it was shadowed the versions differ and xcommit aborts with -2.
//
// Locking: bver_lock protects the version table.  A version is always read
// while the caller holds the block's buffer lock, so a concurrent real write
// (which bumps the version under the same buffer lock) cannot race with the
// check.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define BLOCKVERMAX FSSIZE // every block on the xv6 file system can be tracked

struct blockver {
  uint dev;      // device
  uint blockno;  // block number
  uint version;  // bumped on every real write
  int inuse;     // slot in use
};

static struct spinlock bver_lock;
static struct blockver bvers[BLOCKVERMAX];
static int bver_inited;

void
conflict_init(void)
{
  initlock(&bver_lock, "bver");
  bver_inited = 1;
}

// Find the version slot for (dev, blockno).  Caller holds bver_lock.
// Allocates a slot if none exists (table is small and rarely full).
static struct blockver *
bver_find_locked(uint dev, uint blockno)
{
  struct blockver *free = 0;

  for (struct blockver *b = bvers; b < bvers + BLOCKVERMAX; b++) {
    if (!b->inuse) {
      free = free ? free : b;
      continue;
    }
    if (b->dev == dev && b->blockno == blockno)
      return b;
  }
  if (free) {
    free->inuse = 1;
    free->dev = dev;
    free->blockno = blockno;
    free->version = 0;
    return free;
  }
  return 0;
}

// Bump the version of a block that is being written for real.
// The caller must hold the block's buffer lock (bwrite/log_write do), so no
// other writer can change this block while we bump its version.
void
blockver_touch(uint dev, uint blockno)
{
  if (!bver_inited)
    return;
  acquire(&bver_lock);
  struct blockver *b = bver_find_locked(dev, blockno);
  if (b)
    b->version += 1;
  release(&bver_lock);
}

// Return the current version of a block (0 if never tracked).
// Caller should hold the block's buffer lock to get a stable view.
uint
blockver_version(uint dev, uint blockno)
{
  if (!bver_inited)
    return 0;
  acquire(&bver_lock);
  struct blockver *b = bver_find_locked(dev, blockno);
  uint v = b ? b->version : 0;
  release(&bver_lock);
  return v;
}
