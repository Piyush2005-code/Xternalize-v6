#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define ADMIN_PASSWORD "admin"

static void
trim(char *s)
{
  int len = strlen(s);
  while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
    s[len - 1] = '\0';
    len--;
  }
}

int
main(void)
{
  char user[64];
  char pass[64];
  char *sh_argv[] = {"sh", 0};

  for (;;) {
    printf("\n=== xv6 Login ===\n");
    printf("Login as (admin/user): ");
    memset(user, 0, sizeof(user));
    if (gets(user, sizeof(user)) == 0) {
      exit(0);
    }
    trim(user);

    if (strcmp(user, "admin") == 0) {
      printf("Password: ");
      memset(pass, 0, sizeof(pass));
      if (gets(pass, sizeof(pass)) == 0) {
        exit(0);
      }
      trim(pass);

      if (strcmp(pass, ADMIN_PASSWORD) == 0) {
        if (setuid(0) < 0) {
          printf("login: setuid failed\n");
          exit(1);
        }
        printf("\nWelcome, admin! Starting shell...\n\n");
        exec("sh", sh_argv);
        printf("login: exec sh failed\n");
        exit(1);
      } else {
        printf("Login incorrect: wrong password.\n");
      }
    } else if (strcmp(user, "user") == 0) {
      // Normal user: log in without any password
      if (setuid(1) < 0) {
        printf("login: setuid failed\n");
        exit(1);
      }
      printf("\nWelcome, user! Starting shell...\n\n");
      exec("sh", sh_argv);
      printf("login: exec sh failed\n");
      exit(1);
    } else {
      if (strlen(user) > 0) {
        printf("Unknown user '%s'. Please enter 'admin' or 'user'.\n", user);
      }
    }
  }
}
