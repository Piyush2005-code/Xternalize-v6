#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(void)
{
  int uid = getuid();
  if (uid == 0) {
    printf("admin (uid=0)\n");
  } else if (uid == 1) {
    printf("user (uid=1)\n");
  } else {
    printf("uid=%d\n", uid);
  }
  exit(0);
}
