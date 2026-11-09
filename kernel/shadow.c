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
    if (stable.shadows[i].spec == spec && stable.shadows[i].dev == dev &&
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
// The i-th (0-based, in table order) shadow owned by speculation spec: sets
// *dev, *blockno and *data (a pointer into the table, valid until
// shadow_free_all(spec)) and returns 0; returns -1 when spec owns fewer than
// i+1 shadows.  This is the iterator xcommit uses to walk a speculation's
// blocks; shadow_count() says how many to expect.
int
shadow_get(int spec, int i, uint *dev, uint *blockno, char **data)
{
  int ret = -1;

  acquire(&stable.lock);
  for (int k = 0; k < MAXSHADOWBLOCKS; k++) {
    struct shadow *s = &stable.shadows[k];
    if (s->spec != spec)
      continue;
    if (i-- == 0) {
      *dev = s->dev;
      *blockno = s->blockno;
      *data = s->data;
      ret = 0;
      break;
    }
  }
  release(&stable.lock);
  return ret;
}

// Copy the speculation's shadow of (dev, blockno) into dst, under the table
// lock.  Returns 1 if there is one, 0 if not.  (shadow_find() hands out a
// pointer that outlives the lock; readers that can race with an abort use
// this instead.)
int
shadow_read(int spec, uint dev, uint blockno, char *dst)
{
  int found = 0;

  acquire(&stable.lock);
  for (int i = 0; i < MAXSHADOWBLOCKS; i++) {
    if (stable.shadows[i].spec == spec && stable.shadows[i].dev == dev &&
        stable.shadows[i].blockno == blockno) {
      memmove(dst, stable.shadows[i].data, BSIZE);
      found = 1;
      break;
    }
  }
  release(&stable.lock);
  return found;
}

// Store src as the speculation's version of (dev, blockno): update its
// shadow, or take a free slot for it.  Returns 0, or -1 if the table is full
// (nothing is stored then).  This is what log_write() does for a private
// buffer.
int
shadow_put(int spec, uint dev, uint blockno, char *src)
{
  struct shadow *free = 0;
  int ret = -1;

  acquire(&stable.lock);
  for (int i = 0; i < MAXSHADOWBLOCKS; i++) {
    struct shadow *s = &stable.shadows[i];
    if (s->spec == spec && s->dev == dev && s->blockno == blockno) {
      free = s;
      break;
    }
    if (s->spec == 0 && free == 0)
      free = s;
  }
  if (free != 0) {
    free->spec = spec;
    free->dev = dev;
    free->blockno = blockno;
    memmove(free->data, src, BSIZE);
    ret = 0;
  }
  release(&stable.lock);
  return ret;
}
