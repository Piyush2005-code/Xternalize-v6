struct buf {
  int valid; // has data been read from disk?
  int disk;  // does disk "own" buf?
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  int spec; // 0: a shared cache buffer; else the speculation owning this
            // PRIVATE buffer (bio.c), never on the cache list
  struct buf *prev; // LRU cache list
  struct buf *next;
  uchar data[BSIZE];
};
