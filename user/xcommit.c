// xcommit: end a speculation and release everything it held.
//
//   xcommit <id>

#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  if (argc != 2) {
    fprintf(2, "usage: xcommit <id>\n");
    exit(1);
  }
  if (xjoin(atoi(argv[1])) < 0 || xcommit() < 0) {
    fprintf(2, "xcommit: no open speculation %s\n", argv[1]);
    exit(1);
  }
  printf("speculation %s committed\n", argv[1]);
  exit(0);
}
