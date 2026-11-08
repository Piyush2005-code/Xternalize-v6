// spectest: tests for the externalization barrier (xbegin, xjoin,
// xcommit, xabort) that can be checked from inside xv6.
//
// Held console output can only be seen from outside the machine, so it
// is checked by scripts/tests/test_spec_e2e.py.  Every check here is
// made by the test process itself, which never joins a speculation:
// its own output is never held.

#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"

static int failures;

static void
check(char *name, int ok)
{
  printf("spectest: %s: %s\n", name, ok ? "ok" : "FAILED");
  if (!ok)
    failures++;
}

// Open a speculation in a child that exits and leaves it open.
// Returns its id, or -1.
static int
open_spec(void)
{
  int id;

  if (fork() == 0)
    exit(xbegin());
  wait(&id);
  return id;
}

// End speculation id from outside it: join it in a child, then commit
// or abort.  Returns 0 on success.
static int
end_spec(int id, int commit)
{
  int st;

  if (fork() == 0) {
    if (xjoin(id) < 0)
      exit(1);
    exit((commit ? xcommit() : xabort()) < 0);
  }
  wait(&st);
  return st;
}

// The calls refuse what they do not support, and a speculation that has
// ended cannot be used again.
static void
test_rules(void)
{
  int st;

  if (fork() == 0) {
    int bad = 0;
    int id = xbegin();
    bad |= (id <= 0) << 0;
    bad |= (xbegin() != -1) << 1;  // no nesting
    bad |= (xjoin(id) != -1) << 2; // already inside
    bad |= (xabort() != 0) << 3;
    bad |= (xcommit() != -1) << 4; // no longer speculative
    bad |= (xabort() != -1) << 5;
    bad |= (xjoin(id) != -1) << 6; // it is gone
    exit(bad);
  }
  wait(&st);
  check("xbegin/xjoin/xcommit/xabort refuse misuse", st == 0);
  check("xcommit and xabort fail outside a speculation",
        xcommit() == -1 && xabort() == -1);
  check("xjoin fails for an id that is not open", xjoin(12345) == -1);
}

// A write to a reader outside the speculation arrives only after commit.
static void
test_pipe_commit(void)
{
  int p[2], tcommit, tread;
  char c = 0;

  pipe(p);
  if (fork() == 0) {
    close(p[0]);
    xbegin();
    write(p[1], "x", 1);
    pause(20); // give the reader every chance to see it early
    int t = uptime();
    xcommit();
    exit(t);
  }
  close(p[1]);
  int n = read(p[0], &c, 1);
  tread = uptime();
  close(p[0]);
  wait(&tcommit);
  check("pipe write to an outside reader is held until xcommit",
        n == 1 && c == 'x' && tread >= tcommit);
}

// After xabort the outside reader sees end-of-file and never the data.
static void
test_pipe_abort(void)
{
  int p[2];
  char buf[16];

  pipe(p);
  if (fork() == 0) {
    close(p[0]);
    xbegin();
    write(p[1], "secret", 6);
    xabort();
    exit(0);
  }
  close(p[1]);
  int n = read(p[0], buf, sizeof(buf));
  close(p[0]);
  wait(0);
  check("pipe write dropped by xabort, reader gets end-of-file", n == 0);
}

// Held data keeps the pipe open after the writer has exited, and is
// delivered by whoever commits the speculation later.
static void
test_pipe_late_commit(void)
{
  int p[2], id;
  char buf[8];

  pipe(p);
  if (fork() == 0) {
    close(p[0]);
    int id = xbegin();
    write(p[1], "late", 4);
    exit(id); // the speculation stays open
  }
  close(p[1]);
  wait(&id);
  int ok = end_spec(id, 1) == 0;
  int n = read(p[0], buf, sizeof(buf));
  ok = ok && n == 4 && memcmp(buf, "late", 4) == 0;
  ok = ok && read(p[0], buf, sizeof(buf)) == 0; // then end-of-file
  close(p[0]);
  check("held pipe data outlives its writer and arrives at xcommit", ok);
}

// A pipe created inside the speculation is not held: a pipeline inside
// a speculation must not hang.
static void
test_pipe_inside(void)
{
  int st;

  if (fork() == 0) {
    int q[2];
    char buf[8];
    xbegin();
    pipe(q);
    if (fork() == 0) {
      close(q[0]);
      write(q[1], "ping", 4);
      exit(0);
    }
    close(q[1]);
    int n = read(q[0], buf, sizeof(buf));
    wait(0);
    xabort();
    exit(!(n == 4 && memcmp(buf, "ping", 4) == 0));
  }
  wait(&st);
  check("pipe inside the speculation is not held", st == 0);
}

// When the held-output buffer is full the write fails instead of going
// through: HELDBYTES bytes fit, the next byte does not.
static void
test_byte_limit(void)
{
  int p[2], fit;
  char chunk[128], buf[8];

  memset(chunk, 'a', sizeof(chunk));
  pipe(p);
  if (fork() == 0) {
    close(p[0]);
    xbegin();
    int i = 0;
    while (i <= HELDBYTES / sizeof(chunk) &&
           write(p[1], chunk, sizeof(chunk)) > 0)
      i++;
    xabort();
    exit(i);
  }
  close(p[1]);
  wait(&fit);
  int n = read(p[0], buf, sizeof(buf));
  close(p[0]);
  check("write fails once HELDBYTES are held, nothing leaks",
        fit == HELDBYTES / sizeof(chunk) && n == 0);
}

// Each change of destination starts a new record: NHELD of them fit.
static void
test_record_limit(void)
{
  int a[2], b[2], fit;

  pipe(a);
  pipe(b);
  if (fork() == 0) {
    xbegin();
    int i = 0;
    while (i <= NHELD && write((i % 2) ? b[1] : a[1], "r", 1) == 1)
      i++;
    xabort();
    exit(i);
  }
  close(a[1]);
  close(b[1]);
  wait(&fit);
  close(a[0]);
  close(b[0]);
  check("write fails once NHELD records are held", fit == NHELD);
}

// A normal user cannot join a speculation that admin opened.
static void
test_join_permission(void)
{
  int st, id = open_spec();

  if (fork() == 0) {
    if (setuid(1) < 0)
      exit(2);
    exit(xjoin(id) != -1);
  }
  wait(&st);
  check("user cannot join admin's speculation", id > 0 && st == 0);
  end_spec(id, 0);
}

// At most NSPEC speculations are open at once, and ending one frees its
// slot.
static void
test_slot_limit(void)
{
  int ids[NSPEC + 1], n = 0, ok;

  while (n <= NSPEC && (ids[n] = open_spec()) > 0)
    n++;
  ok = n >= 1 && n <= NSPEC && open_spec() == -1;
  for (int i = 0; i < n; i++)
    ok = ok && end_spec(ids[i], 0) == 0;
  int id = open_spec();
  ok = ok && id > 0 && end_spec(id, 0) == 0;
  check("xbegin fails when all NSPEC slots are open, ending frees one", ok);
}

// A pipe created before xbegin, with both ends then used inside the
// speculation, must not deadlock: the inside reader pulls the held data
// (regression test for the reader-inside hang).
static void
test_reader_inside(void)
{
  int st, p[2];
  char buf[8];

  pipe(p);
  if (fork() == 0) {
    xbegin(); // the pipe and both its ends are now inside
    if (fork() == 0) {
      close(p[0]);
      write(p[1], "ping", 4);
      close(p[1]);
      exit(0);
    }
    int n = read(p[0], buf, sizeof(buf));
    wait(0);
    xabort();
    exit(!(n == 4 && memcmp(buf, "ping", 4) == 0));
  }
  wait(&st);
  close(p[0]);
  close(p[1]);
  check("a reader inside the speculation is not deadlocked", st == 0);
}

// A speculation whose login session has gone away (its members called
// setsid() and exited without committing) is reclaimed on demand by a
// later xbegin, and its held output is dropped, not released.
static void
test_orphan_session(void)
{
  int p[2];
  char buf[8];

  pipe(p);
  for (int i = 0; i < NSPEC; i++) {
    if (fork() == 0) {
      setsid();          // a login session of this child's own
      int id = xbegin(); // owned by a session that is about to vanish
      if (i == 0 && id > 0)
        write(p[1], "x", 1); // held write to an outside reader
      exit(id > 0 ? 0 : 1);
    }
    wait(0);
  }
  close(p[1]);

  // All NSPEC slots are now held by dead sessions.  An xbegin from our
  // own (live) session must reclaim one and succeed.
  int id = open_spec();
  check("a dead session's speculation is reclaimed on demand", id > 0);
  if (id > 0)
    end_spec(id, 0);

  // The reclaimed orphan's held pipe write was dropped: end-of-file.
  int n = read(p[0], buf, sizeof(buf));
  close(p[0]);
  check("a reclaimed speculation's held output is dropped", n == 0);
}

// More held bytes than the pipe buffer, delivered at commit to a reader
// that is a different process from the committer (so neither blocks the
// other).
static void
test_large_pipe_commit(void)
{
  int p[2], id, rst;

  pipe(p);
  if (fork() == 0) { // writer: holds 700 bytes, leaves the speculation open
    close(p[0]);
    int myid = xbegin();
    for (int i = 0; i < 700; i++)
      write(p[1], "z", 1);
    exit(myid);
  }
  wait(&id);
  close(p[1]); // now only held data can reach the reader

  int rpid = fork();
  if (rpid == 0) { // reader: drains the pipe
    int tot = 0, k;
    char buf[64];
    while ((k = read(p[0], buf, sizeof(buf))) > 0)
      tot += k;
    exit(tot == 700 ? 0 : 1);
  }
  close(p[0]);
  end_spec(id,
           1); // a third process joins and commits, delivering to the reader
  wait(&rst);
  check("held pipe data larger than the pipe buffer is delivered at commit",
        id > 0 && rst == 0);
}

int
main(void)
{
  test_rules();
  test_pipe_commit();
  test_pipe_abort();
  test_pipe_late_commit();
  test_pipe_inside();
  test_byte_limit();
  test_record_limit();
  test_join_permission();
  test_slot_limit();
  test_reader_inside();
  test_orphan_session();
  test_large_pipe_commit();

  if (failures == 0)
    printf("spectest: all tests passed\n");
  else
    printf("spectest: %d test(s) FAILED\n", failures);
  exit(failures != 0);
}
