// permtest: tests file permissions and syscall privilege levels.
//
//   permtest            run the case set for the uid it starts with
//   permtest nokill1    same, but skip kill(1): a kernel that lets a user
//                       kill init panics there and takes the run with it
//   permtest setup      (admin) create the fixtures a user run needs
//   permtest teardown   (admin) check the fixtures survived, then remove them
//
// A uid-1 process cannot create admin-owned things, so the fixtures are
// made by an admin: a directory /ptadm (default mode 075) holding /ptadm/s,
// and an admin-owned daemon process whose pid is stored in /ptpid.
// Run as admin with no argument, permtest makes the fixtures itself, runs the
// admin cases, then runs the user cases in a child that dropped to uid 1.
//
// The user cases assume "/" is writable by users (mkfs gives it mode 077).

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/param.h"
#include "kernel/fs.h"
#include "user/user.h"

#define ADMDIR  "/ptadm"
#define PIDFILE "/ptpid"

static int nfail;
static int daemon_child; // pid of the daemon this process forked, if any

static void
check(char *name, int ok)
{
  printf("%s %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok)
    nfail++;
}

// open then close; returns open()'s result.
static int
tryopen(char *path, int mode)
{
  int fd = open(path, mode);
  if (fd >= 0)
    close(fd);
  return fd;
}

static int
writefile(char *path, char *s)
{
  int n = strlen(s);
  int fd = open(path, O_CREATE | O_WRONLY | O_TRUNC);

  if (fd < 0)
    return -1;
  n = (write(fd, s, n) == n) ? 0 : -1;
  close(fd);
  return n;
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

static int
contains(char *s, char *needle)
{
  int i, n = strlen(needle);

  for (i = 0; s[i]; i++)
    if (memcmp(s + i, needle, n) == 0)
      return 1;
  return 0;
}

// Each uid gets its own scratch output file: files are owned by their creator.
static char *
outpath(void)
{
  return getuid() == 0 ? "/ptout0" : "/ptout1";
}

// Run argv[0] through execvp with stdout/stderr in outpath(); returns the
// exit status (127 if exec failed) and leaves the output in buf.
static int
runout(char **argv, char *buf, int n)
{
  int pid, st = -1;

  buf[0] = 0;
  pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    close(1);
    close(2);
    if (open(outpath(), O_CREATE | O_WRONLY | O_TRUNC) < 0)
      exit(126);
    dup(1);
    execvp(argv[0], argv);
    exit(127);
  }
  wait(&st);
  readfile(outpath(), buf, n);
  unlink(outpath());
  return st;
}

static int
daemon_pid(void)
{
  char buf[16];

  if (readfile(PIDFILE, buf, sizeof(buf)) <= 0)
    return 0;
  return atoi(buf);
}

// Remove every entry of directory path, then the directory itself.
static void
rmdir_all(char *path)
{
  char name[MAXPATH];
  struct dirent de;
  int fd, n = strlen(path);

  fd = open(path, O_RDONLY);
  if (fd >= 0) {
    while (read(fd, &de, sizeof(de)) == sizeof(de)) {
      if (de.inum == 0 || strcmp(de.name, ".") == 0 ||
          strcmp(de.name, "..") == 0)
        continue;
      if (n + 1 + DIRSIZ >= MAXPATH)
        continue;
      memmove(name, path, n);
      name[n] = '/';
      memmove(name + n + 1, de.name, DIRSIZ);
      name[n + 1 + DIRSIZ] = 0;
      unlink(name);
    }
    close(fd);
  }
  unlink(path);
}

// Files any run may leave behind; removing them needs the right uid, so
// this is best effort.
static void
scrub(void)
{
  unlink("/ptu_f");
  unlink("/ptu_d/f");
  unlink("/ptu_d");
  unlink("/ptdev");
  unlink("/pta_ls");
  unlink("/pta_dev");
  unlink("/ptout0");
  unlink("/ptout1");
}

static void
setup(void)
{
  int pid, fd, ok;

  rmdir_all(ADMDIR);
  unlink(PIDFILE);
  ok = mkdir(ADMDIR) == 0 && writefile(ADMDIR "/s", "secret\n") == 0;

  // An admin-owned process for the user to (fail to) kill.
  pid = fork();
  if (pid == 0) {
    close(0);
    close(1);
    close(2);
    for (;;)
      pause(1000);
  }
  if (pid < 0) {
    ok = 0;
  } else {
    daemon_child = pid;
    fd = open(PIDFILE, O_CREATE | O_WRONLY | O_TRUNC);
    if (fd < 0) {
      ok = 0;
    } else {
      fprintf(fd, "%d\n", pid);
      close(fd);
    }
  }
  check("setup: fixtures created", ok);
}

static void
teardown(void)
{
  int pid = daemon_pid();

  if (pid <= 0) {
    check("teardown: fixtures present", 0);
  } else if (daemon_child) {
    kill(pid); // our own child: a user kill was already checked in-process
    wait(0);
  } else {
    // Started by a separate 'permtest setup'; init reaps it if it died, so
    // kill() failing means the user's kill attempts got through.
    check("teardown: admin daemon survived the user's kill attempts",
          kill(pid) == 0);
  }
  rmdir_all(ADMDIR);
  unlink(PIDFILE);
  scrub();
}

// Cases for a process with uid 1.
static void
user_cases(int do_kill1)
{
  char buf[256];
  char *ls[] = {"ls", "/", 0};
  char *cat[] = {"cat", "/README", 0};
  char *echo[] = {"echo", "hi", "there", 0};
  int fd, pid, ok, dpid;

  check("user: uid is 1", getuid() == 1);

  // ---- must be denied
  check("user: setuid(0) denied", setuid(0) < 0 && getuid() == 1);
  check("user: open /README O_WRONLY denied", tryopen("/README", O_WRONLY) < 0);
  check("user: open /README O_RDWR denied", tryopen("/README", O_RDWR) < 0);
  check("user: open /ls O_WRONLY denied", tryopen("/ls", O_WRONLY) < 0);
  check("user: mkdir in admin dir denied", mkdir(ADMDIR "/d") < 0);
  check("user: create in admin dir denied",
        tryopen(ADMDIR "/f", O_CREATE | O_WRONLY) < 0);
  check("user: write admin file in admin dir denied",
        tryopen(ADMDIR "/s", O_WRONLY) < 0);
  check("user: link into admin dir denied", link("/README", ADMDIR "/ln") < 0);
  check("user: unlink in admin dir denied", unlink(ADMDIR "/s") < 0);
  dpid = daemon_pid();
  check("user: kill admin-owned process denied", dpid > 0 && kill(dpid) < 0);
  // admin-only syscalls: the privilege table must block them
  check("user: mknod denied (admin-only syscall)", mknod("/ptdev", 1, 1) < 0);
  check("user: sync denied (admin-only syscall)", sync() < 0);
  // last, because they destroy system files on a kernel that allows them
  check("user: unlink /ls denied", unlink("/ls") < 0);
  check("user: unlink /README denied", unlink("/README") < 0);

  // ---- must be allowed
  ok = runout(ls, buf, sizeof(buf)) == 0 && contains(buf, "README");
  check("user: run ls", ok);
  ok = runout(cat, buf, sizeof(buf)) == 0 && contains(buf, "xv6");
  check("user: run cat /README", ok);
  ok = runout(echo, buf, sizeof(buf)) == 0 && strcmp(buf, "hi there\n") == 0;
  check("user: run echo", ok);
  fd = open("/README", O_RDONLY);
  check("user: read /README", fd >= 0 && read(fd, buf, 16) > 0);
  if (fd >= 0)
    close(fd);
  check("user: open /README O_CREATE|O_RDONLY allowed",
        tryopen("/README", O_CREATE | O_RDONLY) >= 0);
  ok = writefile("/ptu_f", "own\n") == 0 &&
       readfile("/ptu_f", buf, sizeof(buf)) == 4 && strcmp(buf, "own\n") == 0 &&
       unlink("/ptu_f") == 0;
  check("user: create, write, read, unlink own file", ok);
  ok = mkdir("/ptu_d") == 0 && writefile("/ptu_d/f", "x") == 0 &&
       unlink("/ptu_d/f") == 0 && unlink("/ptu_d") == 0;
  check("user: mkdir, create in it, unlink own dir", ok);
  pid = fork();
  if (pid == 0) {
    for (;;)
      pause(10);
  }
  check("user: kill own child", pid > 0 && kill(pid) == 0);
  wait(0);

  scrub();

  // Last: a kernel that lets a user kill init panics right here.
  if (do_kill1) {
    printf("note: kill(1) next; a kernel that allows it panics\n");
    check("user: kill(1) denied", kill(1) < 0);
    check("user: system still up after kill(1)", getpid() > 0);
  }
}

// Cases for a process with uid 0.
static void
admin_cases(void)
{
  int pid, st, ok;

  check("admin: uid is 0", getuid() == 0);
  check("admin: setuid(0) allowed", setuid(0) == 0);
  check("admin: open /README O_WRONLY allowed",
        tryopen("/README", O_WRONLY) >= 0);
  check("admin: open /ls O_WRONLY allowed", tryopen("/ls", O_WRONLY) >= 0);
  // unlink of a system program without losing it: unlink a second name
  check("admin: unlink /ls allowed (second link)",
        link("/ls", "/pta_ls") == 0 && unlink("/pta_ls") == 0);
  ok = mkdir(ADMDIR "/d") == 0 && unlink(ADMDIR "/d") == 0;
  check("admin: mkdir and unlink in admin dir", ok);
  ok =
    tryopen(ADMDIR "/f", O_CREATE | O_WRONLY) >= 0 && unlink(ADMDIR "/f") == 0;
  check("admin: create and unlink in admin dir", ok);
  ok = link("/README", ADMDIR "/ln") == 0 && unlink(ADMDIR "/ln") == 0;
  check("admin: link into admin dir", ok);
  check("admin: mknod allowed",
        mknod("/pta_dev", 1, 1) == 0 && unlink("/pta_dev") == 0);
  check("admin: sync allowed", sync() == 0);

  pid = fork();
  if (pid == 0) {
    for (;;)
      pause(10);
  }
  check("admin: kill own child", pid > 0 && kill(pid) == 0);
  wait(0);

  // setuid as login needs it: drop to uid 1 and never get back
  pid = fork();
  if (pid == 0)
    exit(setuid(1) == 0 && getuid() == 1 && setuid(0) < 0 && getuid() == 1 ? 0
                                                                           : 1);
  wait(&st);
  check("admin: setuid(1) works and cannot be undone", st == 0);

  // a uid-1 child that an admin kills
  pid = fork();
  if (pid == 0) {
    setuid(1);
    for (;;)
      pause(10);
  }
  pause(3);
  check("admin: kill a uid-1 process", pid > 0 && kill(pid) == 0);
  wait(0);
  scrub();
}

int
main(int argc, char **argv)
{
  int uid = getuid(), kill1 = 1, pid, st;
  char *mode = argc > 1 ? argv[1] : "";

  if (strcmp(mode, "nokill1") == 0)
    kill1 = 0;

  if (strcmp(mode, "setup") == 0 || strcmp(mode, "teardown") == 0) {
    if (uid != 0) {
      printf("permtest: %s needs admin\n", mode);
      exit(1);
    }
    if (mode[0] == 's')
      setup();
    else
      teardown();
  } else if (uid == 0) {
    setup();
    admin_cases();
    // the user cases, run by a child that dropped to uid 1
    pid = fork();
    if (pid == 0) {
      nfail = 0;
      if (setuid(1) < 0) {
        printf("FAIL admin: setuid(1) for the user cases\n");
        exit(99);
      }
      user_cases(0);
      exit(nfail < 99 ? nfail : 99);
    }
    wait(&st);
    nfail += st;
    teardown();
  } else {
    if (daemon_pid() <= 0 || tryopen(ADMDIR, O_RDONLY) < 0) {
      check("fixtures present (run 'permtest setup' as admin first)", 0);
    } else {
      user_cases(kill1);
    }
  }

  if (nfail == 0) {
    printf("permtest: ALL OK\n");
    exit(0);
  }
  printf("permtest: %d FAILED\n", nfail);
  exit(1);
}
