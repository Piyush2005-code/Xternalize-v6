// demospec: In-guest verification of speculative rm a and echo hi > b.
//
// Demonstrates that:
//   1. Abort leaves no trace: 'a' remains intact, 'b' is never created.
//   2. Commit makes both appear at once: 'a' is deleted, 'b' appears with 'hi'.
//
// Can be run as:
//   demospec         (runs both abort and commit demos)
//   demospec abort   (runs only the abort demo)
//   demospec commit  (runs only the commit demo)

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

static int
file_exists(const char *path)
{
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return 0;
  close(fd);
  return 1;
}

static int
read_file(const char *path, char *buf, int max)
{
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  int n = read(fd, buf, max - 1);
  if (n < 0)
    n = 0;
  buf[n] = 0;
  close(fd);
  return n;
}

static int
write_file(const char *path, const char *content)
{
  int fd = open(path, O_CREATE | O_WRONLY | O_TRUNC);
  if (fd < 0)
    return -1;
  int len = strlen(content);
  if (write(fd, content, len) != len) {
    close(fd);
    return -1;
  }
  close(fd);
  return 0;
}

static int
run_demo(int do_commit)
{
  int id, status;
  char buf[64];

  printf("\n=== %s DEMO: Speculative 'rm a' and 'echo hi > b' ===\n",
         do_commit ? "COMMIT" : "ABORT");

  // Step 1: Initial state
  printf("[1] Setup initial filesystem state...\n");
  unlink("a");
  unlink("b");
  if (write_file("a", "hello\n") < 0) {
    fprintf(2, "demospec: failed to create file 'a'\n");
    return -1;
  }
  if (!file_exists("a") || file_exists("b")) {
    fprintf(2, "demospec: setup failed: a must exist and b must not exist\n");
    return -1;
  }
  printf("    Initial state: file 'a' exists (content: 'hello'), file 'b' does not exist.\n");

  // Step 2: Open speculation
  printf("[2] Opening speculation (xbegin)...\n");
  if (fork() == 0)
    exit(xbegin());
  wait(&id);
  if (id <= 0) {
    fprintf(2, "demospec: xbegin failed\n");
    return -1;
  }
  printf("    Speculation %d opened.\n", id);

  // Step 3: Run speculative operations in member child
  printf("[3] Performing speculative operations inside speculation %d:\n", id);
  printf("    - rm a (unlink)\n");
  printf("    - echo hi > b (create and write)\n");
  if (fork() == 0) {
    if (xjoin(id) < 0) {
      fprintf(2, "demospec child: xjoin failed\n");
      exit(1);
    }
    if (unlink("a") < 0) {
      fprintf(2, "demospec child: speculative unlink(a) failed\n");
      exit(1);
    }
    if (write_file("b", "hi\n") < 0) {
      fprintf(2, "demospec child: speculative write_file(b) failed\n");
      exit(1);
    }
    exit(0);
  }
  wait(&status);
  if (status != 0) {
    fprintf(2, "demospec: speculative child failed with status %d\n", status);
    return -1;
  }

  // Step 4: Verify isolation from outside before ending speculation
  printf("[4] Checking outside view while speculation %d is open:\n", id);
  if (!file_exists("a")) {
    fprintf(2, "FAIL: file 'a' disappeared outside speculation before %s!\n",
            do_commit ? "commit" : "abort");
    return -1;
  }
  read_file("a", buf, sizeof(buf));
  if (strcmp(buf, "hello\n") != 0) {
    fprintf(2, "FAIL: file 'a' content changed outside before %s: got '%s'\n",
            do_commit ? "commit" : "abort", buf);
    return -1;
  }
  if (file_exists("b")) {
    fprintf(2, "FAIL: file 'b' is visible outside before %s!\n",
            do_commit ? "commit" : "abort");
    return -1;
  }
  printf("    ISOLATION OK: outside still sees 'a' intact and 'b' absent.\n");

  // Step 5: End speculation
  if (do_commit) {
    printf("[5] Committing speculation %d (xcommit)...\n", id);
    if (fork() == 0) {
      if (xjoin(id) < 0 || xcommit() < 0)
        exit(1);
      exit(0);
    }
    wait(&status);
    if (status != 0) {
      fprintf(2, "demospec: xcommit failed\n");
      return -1;
    }
  } else {
    printf("[5] Aborting speculation %d (xabort)...\n", id);
    if (fork() == 0) {
      if (xjoin(id) < 0 || xabort() < 0)
        exit(1);
      exit(0);
    }
    wait(&status);
    if (status != 0) {
      fprintf(2, "demospec: xabort failed\n");
      return -1;
    }
  }

  // Step 6: Verify final state
  printf("[6] Verifying final state after %s:\n", do_commit ? "commit" : "abort");
  if (!do_commit) {
    // Abort check: leaves no trace
    if (!file_exists("a")) {
      fprintf(2, "FAIL: file 'a' was deleted despite abort!\n");
      return -1;
    }
    read_file("a", buf, sizeof(buf));
    if (strcmp(buf, "hello\n") != 0) {
      fprintf(2, "FAIL: file 'a' content corrupted after abort: '%s'\n", buf);
      return -1;
    }
    if (file_exists("b")) {
      fprintf(2, "FAIL: file 'b' was created despite abort!\n");
      return -1;
    }
    printf("    RESULT: 'a' is preserved, 'b' was never created.\n");
    printf(">>> PASS: Abort left no trace! <<<\n");
  } else {
    // Commit check: both appear at once
    if (file_exists("a")) {
      fprintf(2, "FAIL: file 'a' was not deleted after commit!\n");
      return -1;
    }
    if (!file_exists("b")) {
      fprintf(2, "FAIL: file 'b' was not created after commit!\n");
      return -1;
    }
    read_file("b", buf, sizeof(buf));
    if (strcmp(buf, "hi\n") != 0) {
      fprintf(2, "FAIL: file 'b' content incorrect after commit: got '%s', expected 'hi\\n'\n", buf);
      return -1;
    }
    printf("    RESULT: 'a' was deleted, 'b' was created with 'hi'.\n");
    printf(">>> PASS: Commit made both changes appear at once! <<<\n");
  }

  // Clean up
  unlink("a");
  unlink("b");
  return 0;
}

int
main(int argc, char *argv[])
{
  int fail = 0;

  if (argc > 1 && strcmp(argv[1], "abort") == 0) {
    if (run_demo(0) < 0)
      fail++;
  } else if (argc > 1 && strcmp(argv[1], "commit") == 0) {
    if (run_demo(1) < 0)
      fail++;
  } else {
    // Run both
    if (run_demo(0) < 0)
      fail++;
    if (run_demo(1) < 0)
      fail++;
  }

  if (fail == 0) {
    printf("\nAll demo checks passed successfully.\n");
    exit(0);
  } else {
    printf("\nDemo failed with %d error(s).\n", fail);
    exit(1);
  }
}
