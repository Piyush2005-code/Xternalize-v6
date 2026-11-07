// xbegin: open a speculation and print its id.
//
// Run commands inside it with "xrun <id> <command>", then end it with
// "xcommit <id>" or "xabort <id>".  The shell stays outside the
// speculation, so its prompt is never held.

#include "kernel/types.h"
#include "user/user.h"

int
main(void)
{
  int id;

  // A child opens the speculation and exits, leaving it open.  If this
  // process opened it, the line printed below would be held too.
  if (fork() == 0)
    exit(xbegin());
  wait(&id);
  if (id <= 0) {
    fprintf(2, "xbegin: cannot open a speculation\n");
    exit(1);
  }
  printf("speculation %d\n", id);
  exit(0);
}
