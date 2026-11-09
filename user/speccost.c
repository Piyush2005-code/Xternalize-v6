// speccost: what does a file write cost inside a speculation?
//
//   speccost <mode> <kb> <reps>
//
// Overwrites the first <kb> KB of /sc_cost (made beforehand, 64 KB), <reps>
// times, and prints the uptime ticks it took:
//   out     plain writes, no speculation
//   in      writes inside one speculation (kept in shadow blocks), then xabort
//   commit  each repetition is xbegin, the writes, xcommit
//   abort   each repetition is xbegin, the writes, xabort
// fails counts repetitions where a write returned -1 (the shadow table ran
// out).  scripts/bench_spec_cost.py drives it.

#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

static char blk[1024];

static int
writes(int kb)
{
  int fd = open("/sc_cost", O_WRONLY);
  int bad = 0;

  if (fd < 0)
    return 1;
  for (int k = 0; k < kb; k++)
    if (write(fd, blk, sizeof(blk)) != sizeof(blk)) {
      bad = 1;
      break;
    }
  close(fd);
  return bad;
}

int
main(int argc, char **argv)
{
  int kb, reps, fails = 0, t0, t1;
  char *mode;

  if (argc != 4) {
    fprintf(2, "usage: speccost out|in|commit|abort KB REPS\n");
    exit(1);
  }
  mode = argv[1];
  kb = atoi(argv[2]);
  reps = atoi(argv[3]);
  memset(blk, 'x', sizeof(blk));

  t0 = uptime();
  if (strcmp(mode, "in") == 0) {
    if (xbegin() < 0)
      exit(2);
    for (int r = 0; r < reps; r++)
      fails += writes(kb);
    xabort();
  } else {
    for (int r = 0; r < reps; r++) {
      if (strcmp(mode, "out") != 0 && xbegin() < 0)
        exit(2);
      fails += writes(kb);
      if (strcmp(mode, "commit") == 0)
        xcommit();
      else if (strcmp(mode, "abort") == 0)
        xabort();
    }
  }
  t1 = uptime();
  printf("speccost: mode=%s kb=%d reps=%d ticks=%d fails=%d\n", mode, kb, reps,
         t1 - t0, fails);
  exit(0);
}
