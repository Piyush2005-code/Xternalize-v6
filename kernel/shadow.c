// Speculative shadow table.
//
// A fixed set of shadow copies of disk blocks, kept entirely in memory
// and protected by a spinlock. Shadows are keyed by (spec, dev, blockno),
// not by inode. A speculative process reads and writes through its shadows;
// nothing is staged into the real log until the transaction commits.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "buf.h"

struct {
  struct spinlock lock;
  struct shadow shadows[MAXSHADOWBLOCKS];
} stable;

void
shadow_init(void)
{
  initlock(&stable.lock, "shadow");
  // The shadow array is zero-initialized: spec == 0 marks a free slot,
  // so no per-entry initialization is required.
}

// Find the shadow for speculative process spec holding block (dev, blockno).
// Returns 0 if none exists.
struct shadow *
shadow_find(int spec, uint dev, uint blockno)
{
  struct shadow *s = 0;

  acquire(&stable.lock);
  for (int i = 0; i < MAXSHADOWBLOCKS; i++) {
    if (stable.shadows[i].spec == spec &&
        stable.shadows[i].dev == dev &&
        stable.shadows[i].blockno == blockno) {
      s = &stable.shadows[i];
      break;
    }
  }
  release(&stable.lock);
  return s;
}

// Allocate a shadow for (spec, dev, blockno) and copy the current
// contents of b into it. Returns 0 if the table is full.
struct shadow *
shadow_alloc(int spec, uint dev, uint blockno, struct buf *b)
{
  struct shadow *s = 0;

  acquire(&stable.lock);
  for (int i = 0; i < MAXSHADOWBLOCKS; i++) {
    if (stable.shadows[i].spec == 0) { // free slot
      stable.shadows[i].spec = spec;
      stable.shadows[i].dev = dev;
      stable.shadows[i].blockno = blockno;
      memmove(stable.shadows[i].data, b->data, BSIZE);
      s = &stable.shadows[i];
      break;
    }
  }
  release(&stable.lock);
  return s;
}

// Drop every shadow owned by speculative process spec (called on abort).
void
shadow_free_all(int spec)
{
  acquire(&stable.lock);
  for (int i = 0; i < MAXSHADOWBLOCKS; i++) {
    if (stable.shadows[i].spec == spec)
      stable.shadows[i].spec = 0;
  }
  release(&stable.lock);
}

// Return the number of shadows owned by speculative process spec.
int
shadow_count(int spec)
{
  int n = 0;

  acquire(&stable.lock);
  for (int i = 0; i < MAXSHADOWBLOCKS; i++) {
    if (stable.shadows[i].spec == spec)
      n++;
  }
  release(&stable.lock);
  return n;
}