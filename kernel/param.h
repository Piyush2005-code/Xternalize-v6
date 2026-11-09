#define NPROC       64                // maximum number of processes
#define NCPU        8                 // maximum number of CPUs
#define NOFILE      16                // open files per process
#define NFILE       100               // open files per system
#define NINODE      50                // maximum number of active i-nodes
#define NDEV        10                // maximum major device number
#define ROOTDEV     1                 // device number of file system root disk
#define MAXARG      32                // max exec arguments
#define MAXOPBLOCKS 10                // max # of blocks any FS op writes
#define SPECBLOCKS  64                // blocks one xcommit logs at once (kernel/speccommit.c)
#define LOGBLOCKS   (SPECBLOCKS + MAXOPBLOCKS * 2) // max data blocks in on-disk log
#define NBUF        (LOGBLOCKS + MAXOPBLOCKS)      // size of disk block cache
#define FSSIZE      2000              // size of file system in blocks
#define MAXPATH     128               // maximum file path name
#define USERSTACK   1                 // user stack pages

// Externalization barrier (kernel/spec.c)
#define NSPEC     4    // speculations that can be open at once
#define NHELD     64   // held writes (records) per speculation
#define HELDBYTES 4096 // bytes of output one speculation can hold back
