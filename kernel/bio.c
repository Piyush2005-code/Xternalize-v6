// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"
#include "proc.h"

struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head;
} bcache;

static void pcache_init(void);

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");
  pcache_init();

  // Create linked list of buffers
  bcache.head.prev = &bcache.head;
  bcache.head.next = &bcache.head;
  for (b = bcache.buf; b < bcache.buf + NBUF; b++) {
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    initsleeplock(&b->lock, "buffer");
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf *
bget(uint dev, uint blockno)
{
  struct buf *b;

  acquire(&bcache.lock);

  // Is the block already cached?
  for (b = bcache.head.next; b != &bcache.head; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for (b = bcache.head.prev; b != &bcache.head; b = b->prev) {
    if (b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block, straight
// from the shared cache and the disk.  Never redirected to a shadow: the log
// and the commit of a speculation use this, whoever is running them.
struct buf *
bread_raw(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if (!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// ---- Private buffers for speculative processes ----
//
// Invariant: a speculative process never gets, and so never modifies, a
// buffer of the shared cache; and nobody else ever sees a speculative
// change.  xv6 code does bread(); edit bp->data; log_write(bp); brelse(bp),
// so the edit would land in the shared cache before log_write() could
// redirect it.  Instead bread() by a speculative process returns one of
// these private bufs, filled from that speculation's shadow of the block if
// it has one and from the real block otherwise.
//
// A private buf is keyed by (spec, dev, blockno) and has its own sleeplock,
// so the members of one speculation exclude each other on a block exactly as
// they would on a cache buffer (balloc and ialloc rely on that).  It lives
// only while somebody holds it: log_write() copies the data into the shadow
// (shadow.c), and when the last holder calls brelse() the entry is free
// again.  So a block that is only READ never takes a shadow slot; a slot is
// kept only for blocks that were written (log_write()).
#define NPRIVBUF 64
static struct {
  struct spinlock lock;
  struct buf buf[NPRIVBUF];
} pcache;

static void
pcache_init(void)
{
  initlock(&pcache.lock, "pcache");
  for (int i = 0; i < NPRIVBUF; i++)
    initsleeplock(&pcache.buf[i].lock, "pbuf");
}

static struct buf *
pbread(int spec, uint dev, uint blockno)
{
  struct buf *b, *e = pcache.buf + NPRIVBUF;

  acquire(&pcache.lock);
  for (;;) {
    for (b = pcache.buf; b < e; b++) {
      if (b->refcnt > 0 && b->spec == spec && b->dev == dev &&
          b->blockno == blockno) {
        b->refcnt++;
        release(&pcache.lock);
        acquiresleep(&b->lock);
        return b;
      }
    }
    for (b = pcache.buf; b < e; b++) {
      if (b->refcnt == 0) {
        b->spec = spec;
        b->dev = dev;
        b->blockno = blockno;
        b->refcnt = 1;
        // refcnt 0 means every earlier holder has released the sleeplock,
        // so this cannot block while we hold the spinlock.
        acquiresleep(&b->lock);
        release(&pcache.lock);
        // Fill while holding the sleeplock: later holders wait for it.
        if (!shadow_read(spec, dev, blockno, (char *)b->data, &b->version)) {
          struct buf *r = bread_raw(dev, blockno);
          memmove(b->data, r->data, BSIZE);
          b->version = blockver_version(dev, blockno);
          brelse(r);
        }
        b->valid = 1;
        return b;
      }
    }
    // All private buffers are in use (each holder needs only a few).
    release(&pcache.lock);
    yield();
    acquire(&pcache.lock);
  }
}

static void
pbrelse(struct buf *b)
{
  releasesleep(&b->lock);
  acquire(&pcache.lock);
  if (--b->refcnt == 0)
    b->spec = 0;
  release(&pcache.lock);
}

// Return a locked buf with the contents of the indicated block.  A
// speculative process gets a private buf (see above).
struct buf *
bread(uint dev, uint blockno)
{
  struct proc *p = myproc();

  if (p != 0 && p->spec != 0)
    return pbread(p->spec, dev, blockno);
  return bread_raw(dev, blockno);
}

// Write b's contents to disk.  Must be locked.
// Only the log calls bwrite.
void
bwrite(struct buf *b)
{
  if (!holdingsleep(&b->lock) || b->spec != 0)
    panic("bwrite"); // private buffers are never written to disk
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if (!holdingsleep(&b->lock))
    panic("brelse");
  if (b->spec != 0) { // private buffer of a speculation
    pbrelse(b);
    return;
  }

  releasesleep(&b->lock);

  acquire(&bcache.lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }

  release(&bcache.lock);
}

void
bpin(struct buf *b)
{
  acquire(&bcache.lock);
  b->refcnt++;
  release(&bcache.lock);
}

void
bunpin(struct buf *b)
{
  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}
