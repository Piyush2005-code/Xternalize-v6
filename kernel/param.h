#ifndef PARAM_H
#define PARAM_H

#include "types.h"
#include "fs.h"

#define NPROC       64                // maximum number of processes
#define NCPU        8                 // maximum number of CPUs
#define NOFILE      16                // open files per process
#define NFILE       100               // open files per system
#define NINODE      50                // maximum number of active i-nodes
#define NDEV        10                // maximum major device number
#define ROOTDEV     1                 // device number of file system root disk
#define MAXARG      32                // max exec arguments
#define MAXOPBLOCKS 10                // max # of blocks any FS op writes
#define SPECBLOCKS  64                // blocks one xcommit logs at once
#define LOGBLOCKS   (SPECBLOCKS + 20) // on-disk log: + 2 * MAXOPBLOCKS
#define NBUF        (LOGBLOCKS + 10)  // buffer cache: log + MAXOPBLOCKS
#define FSSIZE      2000              // size of file system in blocks
#define MAXPATH     128               // maximum file path name
#define USERSTACK   1                 // user stack pages

// Externalization barrier (kernel/spec.c)
#define NSPEC     4    // speculations that can be open at once
#define NHELD     64   // held writes (records) per speculation
#define HELDBYTES 4096 // bytes of output one speculation can hold back

#define MAXSHADOWBLOCKS 64 // max speculative shadow blocks in memory

// A speculative shadow copy of a disk block, kept entirely in memory
// and protected by the shadow table's spinlock. Shadows are keyed by
// (spec, dev, blockno), not by inode.
struct shadow {
  int spec; // id of the speculative process that owns this shadow (0 = free)
  uint dev; // device of the block
  uint blockno;     // block number on the device
  char data[BSIZE]; // shadowed block contents
};

#endif // PARAM_H
