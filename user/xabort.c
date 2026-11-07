// xabort: end a speculation and drop everything it held.
//
//   xabort <id>

#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  if (argc != 2) {
    fprintf(2, "usage: xabort <id>\n");
    exit(1);
  }
  if (xjoin(atoi(argv[1])) < 0 || xabort() < 0) {
    fprintf(2, "xabort: no open speculation %s\n", argv[1]);
    exit(1);
  }
  printf("speculation %s aborted\n", argv[1]);
  exit(0);
}
