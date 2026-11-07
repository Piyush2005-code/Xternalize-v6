// xrun: run one command inside an open speculation.
//
//   xrun <id> <command> [args ...]
//
// The command's output, and its writes to pipes whose reader is outside
// the speculation, are held until "xcommit <id>".

#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int status;

  if (argc < 3) {
    fprintf(2, "usage: xrun <id> <command> [args ...]\n");
    exit(1);
  }

  // Only the child joins, so this process can still report errors.
  if (fork() == 0) {
    if (xjoin(atoi(argv[1])) < 0)
      exit(126);
    execvp(argv[2], argv + 2);
    exit(127);
  }
  wait(&status);
  if (status == 126)
    fprintf(2, "xrun: no open speculation %s\n", argv[1]);
  else if (status == 127)
    fprintf(2, "xrun: exec %s failed\n", argv[2]);
  exit(status);
}
