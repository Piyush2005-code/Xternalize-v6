// pathtest: tests execvp() command path resolution from a deep cwd.
//
// Each case forks; the child calls execvp(), the parent checks how
// the child ended.  A child whose execvp() returned exits with
// EXECFAIL, so "program was found and started" is anything else.
// The bare-name sweep covers every program in UPROGS (user/uprogs.h
// is generated from the Makefile).

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/param.h"
#include "kernel/fs.h"
#include "user/user.h"
#include "user/uprogs.h"

#define EXECFAIL 127 // child: execvp() returned -1
#define TIMEOUT  30  // ticks before a runaway program is killed

#define ROOT "/pt"
#define DEEP "/pt/a/b"
#define INFILE  "/pt/in"
#define OUTFILE "/pt/out"

static int nfail;

static void
report(char *name, int ok, char *note)
{
  if (!ok)
    nfail++;
  printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name, note ? " " : "", note ? note : "");
}

static void
writefile(char *path, char *s)
{
  int fd = open(path, O_CREATE | O_WRONLY | O_TRUNC);
  if (fd >= 0) {
    write(fd, s, strlen(s));
    close(fd);
  }
}

// read up to n-1 bytes of path into buf, NUL-terminated; -1 if unopenable.
static int
readfile(char *path, char *buf, int n)
{
  int fd, m, tot = 0;

  fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  while (tot < n - 1 && (m = read(fd, buf + tot, n - 1 - tot)) > 0)
    tot += m;
  close(fd);
  buf[tot] = 0;
  return tot;
}

// Recursively remove path (a file or a directory tree).
static void
rmtree(char *path)
{
  char buf[MAXPATH];
  struct stat st;
  struct dirent de;
  int fd, n;

  if (stat(path, &st) < 0)
    return;
  if (st.type == T_DIR) {
    fd = open(path, O_RDONLY);
    if (fd >= 0) {
      n = strlen(path);
      while (read(fd, &de, sizeof(de)) == sizeof(de)) {
        if (de.inum == 0 || strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
          continue;
        if (n + 1 + DIRSIZ >= MAXPATH)
          continue;
        memmove(buf, path, n);
        buf[n] = '/';
        memmove(buf + n + 1, de.name, DIRSIZ);
        buf[n + 1 + DIRSIZ] = 0;
        rmtree(buf);
      }
      close(fd);
    }
  }
  unlink(path);
}

// Run execvp(file, argv) in a child with stdin from INFILE and
// stdout/stderr to OUTFILE.  Returns the child's exit status
// (EXECFAIL if execvp returned).  *timedout is set if a watchdog had
// to kill it.
static int
run(char *file, char **argv, int *timedout)
{
  int rpid, wpid, p, st, rst = 0, fired = 0, got = 0;

  *timedout = 0;
  rpid = fork();
  if (rpid < 0)
    return -2;
  if (rpid == 0) {
    close(0);
    open(INFILE, O_RDONLY);
    close(1);
    open(OUTFILE, O_CREATE | O_WRONLY | O_TRUNC);
    close(2);
    dup(1);
    execvp(file, argv);
    exit(EXECFAIL);
  }
  wpid = fork();
  if (wpid == 0) {
    pause(TIMEOUT);
    kill(rpid);
    exit(0);
  }
  if (wpid < 0) {
    kill(rpid);
    wait(0);
    return -2;
  }
  while (!got) {
    p = wait(&st);
    if (p == rpid) {
      got = 1;
      rst = st;
    } else if (p == wpid) {
      fired = 1;
    } else if (p < 0) {
      break;
    }
  }
  if (!fired) {
    kill(wpid);
    wait(0);
  }
  *timedout = fired;
  return rst;
}

// Expect execvp(file, argv) to fail and return -1.
static void
expectfail(char *name, char *file, char **argv)
{
  int to, st = run(file, argv, &to);
  report(name, st == EXECFAIL, 0);
}

static void
sweep(void)
{
  char name[32];
  char *argv[3];
  int i, to, st;

  for (i = 0; uprogs[i]; i++) {
    argv[0] = uprogs[i];
    argv[1] = 0;
    argv[2] = 0;
    // init respawns a shell forever; grind never exits and litters the fs.
    if (strcmp(uprogs[i], "init") == 0 || strcmp(uprogs[i], "grind") == 0) {
      printf("SKIP bare:%s (never exits)\n", uprogs[i]);
      continue;
    }
    // These would run their full workload (or recurse), so ask for the
    // quick path instead.
    if (strcmp(uprogs[i], "usertests") == 0)
      argv[1] = "-x"; // prints usage, exits
    if (strcmp(uprogs[i], "pathtest") == 0)
      argv[1] = "--probe"; // exits at once
    name[0] = 0;
    memmove(name, "bare:", 6);
    if (5 + strlen(uprogs[i]) < sizeof(name))
      strcpy(name + 5, uprogs[i]);
    st = run(uprogs[i], argv, &to);
    report(name, st != EXECFAIL && st != -2, to ? "(started, killed after timeout)" : 0);
  }
}

int
main(int argc, char **argv)
{
  char buf[256], longname[MAXPATH + 16];
  char *args[8];
  struct stat sst;
  int i, to, st;

  if (argc == 2 && strcmp(argv[1], "--probe") == 0)
    exit(0);

  // Start from a clean slate in case an earlier run was interrupted.
  chdir("/");
  rmtree(ROOT);

  if (mkdir(ROOT) < 0 || mkdir(ROOT "/a") < 0 || mkdir(DEEP) < 0 || chdir(DEEP) < 0) {
    printf("pathtest: cannot set up %s\n", DEEP);
    chdir("/");
    rmtree(ROOT);
    exit(1);
  }
  writefile(INFILE, "");

  // 1. Every program in UPROGS by bare name, from three directories down.
  sweep();

  // 2. Absolute path.
  args[0] = "echo"; args[1] = "abs"; args[2] = 0;
  st = run("/echo", args, &to);
  readfile(OUTFILE, buf, sizeof(buf));
  report("absolute:/echo", st == 0 && strcmp(buf, "abs\n") == 0, 0);

  // 3. Names with a slash are never searched.
  args[0] = "x"; args[1] = 0;
  expectfail("slash:./nosuch", "./nosuch", args);
  args[0] = "echo";
  expectfail("slash:./echo-no-fallback", "./echo", args);
  expectfail("slash:a/echo-no-fallback", "a/echo", args);

  // 4. Failures: nonexistent, empty, NULL.
  args[0] = "nosuchcmd";
  expectfail("nonexistent", "nosuchcmd", args);
  args[0] = "";
  expectfail("empty-string", "", args);
  expectfail("null-file", 0, args);

  // 5. Long names: MAXPATH-1 chars (fits as given, not as "/"+name),
  // and longer than MAXPATH.
  for (i = 0; i < MAXPATH + 15; i++)
    longname[i] = 'a';
  longname[MAXPATH + 15] = 0;
  args[0] = longname;
  longname[MAXPATH - 1] = 0;
  expectfail("longname:MAXPATH-1", longname, args);
  longname[MAXPATH - 1] = 'a';
  longname[MAXPATH + 5] = 0;
  expectfail("longname:MAXPATH+5", longname, args);

  // 6. A non-ELF file in the cwd named like a program: exec fails on
  // it, so execvp must fall back to /cat.
  writefile("cat", "this is not an ELF file\n");
  writefile(INFILE, "");
  writefile("/pt/data", "from-real-cat\n");
  args[0] = "cat"; args[1] = "/pt/data"; args[2] = 0;
  st = run("cat", args, &to);
  readfile(OUTFILE, buf, sizeof(buf));
  report("cwd-nonelf-fallback", st == 0 && strcmp(buf, "from-real-cat\n") == 0, 0);
  unlink("cat");

  // 7. A command that changes the fs, from three directories down.
  args[0] = "mkdir"; args[1] = "made"; args[2] = 0;
  st = run("mkdir", args, &to);
  report("nested:mkdir", st == 0 && stat("made", &sst) == 0 && sst.type == T_DIR, 0);

  // 8. argv is passed through unchanged (including empty and spaced args).
  args[0] = "echo"; args[1] = "alpha"; args[2] = "be ta"; args[3] = ""; args[4] = "-n"; args[5] = 0;
  st = run("echo", args, &to);
  readfile(OUTFILE, buf, sizeof(buf));
  report("argv-passthrough", st == 0 && strcmp(buf, "alpha be ta  -n\n") == 0, 0);

  // 9. Same from the root directory.
  chdir("/");
  args[0] = "echo"; args[1] = "root"; args[2] = 0;
  st = run("echo", args, &to);
  readfile(OUTFILE, buf, sizeof(buf));
  report("cwd-root:echo", st == 0 && strcmp(buf, "root\n") == 0, 0);

  chdir("/");
  rmtree(ROOT);
  if (stat(ROOT, &sst) == 0)
    report("cleanup", 0, "(/pt still exists)");

  if (nfail == 0) {
    printf("pathtest: ALL OK\n");
    exit(0);
  }
  printf("pathtest: %d FAILED\n", nfail);
  exit(1);
}
