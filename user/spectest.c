// spectest: tests for the externalization barrier (xbegin, xjoin,
// xcommit, xabort) that can be checked from inside xv6.
//
// Held console output can only be seen from outside the machine, so it
// is checked by scripts/tests/test_spec_e2e.py.  Every check here is
// made by the test process itself, which never joins a speculation:
// its own output is never held.

#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
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

// ---- file effects (need the shadow table) ----------------------------------

static int have_shadows; // do file writes inside a speculation stay held?

static void
skip(char *name)
{
  printf("spectest: SKIP %s (needs shadow table)\n", name);
}

// Replace the contents of path with s.
static void
put(char *path, char *s)
{
  int fd = open(path, O_CREATE | O_WRONLY | O_TRUNC);

  if (fd >= 0) {
    write(fd, s, strlen(s));
    close(fd);
  }
}

// Does path exist and hold exactly s?
static int
holds(char *path, char *s)
{
  char buf[64];
  int fd = open(path, O_RDONLY), n;

  if (fd < 0)
    return 0;
  n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n < 0)
    return 0;
  buf[n] = 0;
  return strcmp(buf, s) == 0;
}

static int
exists(char *path)
{
  struct stat st;

  return stat(path, &st) == 0;
}

static int
fsize(char *path)
{
  struct stat st;

  return stat(path, &st) == 0 ? (int)st.size : -1;
}

// Write n bytes of c to path (truncating it), 1000 at a time.
static void
fill_file(char *path, char c, int n)
{
  char buf[1000];
  int fd = open(path, O_CREATE | O_WRONLY | O_TRUNC);

  memset(buf, c, sizeof(buf));
  for (int done = 0; fd >= 0 && done < n; done += 1000)
    write(fd, buf, n - done < 1000 ? n - done : 1000);
  if (fd >= 0)
    close(fd);
}

// Without a shadow table a speculative write goes straight to disk, so
// write a probe file inside a speculation and see whether an outsider can
// see it before the abort.
static void
probe_shadows(void)
{
  int id = open_spec();

  put("/sc_probe", "old");
  if (fork() == 0) {
    int fd;

    if (xjoin(id) < 0)
      exit(1);
    fd = open("/sc_probe", O_WRONLY);
    write(fd, "new", 3);
    close(fd);
    exit(0);
  }
  wait(0);
  have_shadows = holds("/sc_probe", "old");
  end_spec(id, 0);
  unlink("/sc_probe");
}

// A speculative child unlinks /sc_a, overwrites the start of /sc_b,
// creates /sc_c and writes to a pipe whose reader is outside.  Nothing may
// be visible until commit; abort must leave everything as it was.
static void
test_files(int commit)
{
  int p[2], id, rpid, rst;

  put("/sc_a", "A-old");
  put("/sc_b", "B-old");
  unlink("/sc_c");
  pipe(p);
  id = open_spec();
  if (fork() == 0) {
    int fd;

    close(p[0]);
    if (xjoin(id) < 0)
      exit(1);
    unlink("/sc_a");
    fd = open("/sc_b", O_WRONLY);
    write(fd, "hi", 2);
    close(fd);
    put("/sc_c", "cc");
    write(p[1], "out", 3); // held, like console output
    exit(0);
  }
  wait(0);
  close(p[1]);

  check(commit ? "files before commit: nothing visible outside"
               : "files before abort: nothing visible outside",
        exists("/sc_a") && holds("/sc_b", "B-old") && !exists("/sc_c"));

  // The reader is outside; when it gets the output the files must already
  // be changed (output is released only after the files are committed).
  rpid = fork();
  if (rpid == 0) {
    char buf[8];
    int n = read(p[0], buf, sizeof(buf));

    if (commit)
      exit(n == 3 && !exists("/sc_a") && holds("/sc_b", "hiold") &&
               holds("/sc_c", "cc")
             ? 0
             : 1);
    exit(n == 0 ? 0 : 1); // abort: only end-of-file
  }
  close(p[0]);
  end_spec(id, commit);
  wait(&rst);

  if (commit) {
    check("files commit: unlink, overwrite and create all applied",
          !exists("/sc_a") && holds("/sc_b", "hiold") && holds("/sc_c", "cc"));
    check("files commit: output arrives only after the files changed",
          rst == 0);
  } else {
    check("files abort: unlink, overwrite and create all undone",
          holds("/sc_a", "A-old") && holds("/sc_b", "B-old") &&
            !exists("/sc_c"));
    check("files abort: held output dropped", rst == 0);
  }
  unlink("/sc_a");
  unlink("/sc_b");
  unlink("/sc_c");
}

// Isolation: while the speculation is open an outside process sees the
// OLD size, data and directory (neither the shared buffer cache nor the
// in-memory inodes hold anything speculative); a file created and unlinked
// inside never shows up; and after xabort the cache and inodes are exactly as
// before, so ordinary use afterwards (truncate, rewrite, unlink: all of which
// would panic or corrupt on a stale inode) works.
static void
test_isolation(int commit)
{
  int id, st;

  fill_file("/sc_g", 'g', 3000);
  put("/sc_m", "old-data-1");
  unlink("/sc_n");
  unlink("/sc_k");
  id = open_spec();
  if (fork() == 0) {
    if (xjoin(id) < 0)
      exit(1);
    fill_file("/sc_g", 'N', 8000); // grows the file, allocates blocks
    put("/sc_m", "NEW-DATA-9");
    put("/sc_n", "n"); // created and unlinked inside
    unlink("/sc_n");
    put("/sc_k", "k");
    exit(fsize("/sc_g") == 8000 && holds("/sc_m", "NEW-DATA-9") &&
             !exists("/sc_n") && holds("/sc_k", "k")
           ? 0
           : 2);
  }
  wait(&st);
  check("isolation: the member sees its own changes", st == 0);
  check("isolation: outside sees the old size (stat)", fsize("/sc_g") == 3000);
  check("isolation: outside sees the old data", holds("/sc_m", "old-data-1"));
  check("isolation: create+unlink inside is invisible outside",
        !exists("/sc_n") && !exists("/sc_k"));

  end_spec(id, commit);
  if (commit) {
    check("isolation: commit makes the new size and data visible",
          fsize("/sc_g") == 8000 && holds("/sc_m", "NEW-DATA-9") &&
            holds("/sc_k", "k"));
    check("isolation: created-and-unlinked file stays gone", !exists("/sc_n"));
  } else {
    check("isolation: abort leaves the old size, data and directory",
          fsize("/sc_g") == 3000 && holds("/sc_m", "old-data-1") &&
            !exists("/sc_k") && !exists("/sc_n"));
  }
  // Use the files normally afterwards: rewrite shorter (itrunc frees the
  // real blocks), grow again, then unlink.  A stale inode would free blocks
  // that were never allocated and panic here.
  fill_file("/sc_g", 'x', 100);
  check("isolation: rewrite after the speculation works",
        fsize("/sc_g") == 100);
  fill_file("/sc_g", 'y', 6000);
  check("isolation: growing again after the speculation works",
        fsize("/sc_g") == 6000);
  unlink("/sc_g");
  unlink("/sc_m");
  unlink("/sc_k");
  check("isolation: files unlink cleanly afterwards",
        !exists("/sc_g") && !exists("/sc_m") && !exists("/sc_k"));
}

// Reading takes no shadow slot: a slot is kept only for a block that was
// written.  Read a 40 KB file inside a speculation, then see how many 1 KB
// writes still fit (about 59 with nothing read first).
static void
test_read_takes_no_slot(void)
{
  char blk[1024];
  int id, st;

  fill_file("/sc_r", 'r', 40000);
  unlink("/sc_w");
  id = open_spec();
  if (fork() == 0) {
    int fd, n = 0, tot = 0;

    if (xjoin(id) < 0)
      exit(255);
    fd = open("/sc_r", O_RDONLY);
    while ((n = read(fd, blk, sizeof(blk))) > 0)
      tot += n;
    close(fd);
    fd = open("/sc_w", O_CREATE | O_WRONLY);
    for (n = 0; n < 100; n++)
      if (write(fd, blk, sizeof(blk)) != sizeof(blk))
        break;
    close(fd);
    exit(tot == 40000 ? n : 200);
  }
  wait(&st);
  printf("spectest: %d writes fit after reading 40 blocks\n", st);
  check("reading 40 blocks inside a speculation burned no shadow slots",
        st >= 50 && st <= 100);
  end_spec(id, 0);
  unlink("/sc_r");
  unlink("/sc_w");
}

// Inside the speculation a write is read back; outside it is not seen.
static void
test_read_own_writes(void)
{
  int id, st;

  put("/sc_b", "B-old");
  id = open_spec();
  if (fork() == 0) {
    int fd;

    if (xjoin(id) < 0)
      exit(2);
    fd = open("/sc_b", O_WRONLY);
    write(fd, "NEW", 3);
    close(fd);
    exit(holds("/sc_b", "NEWld") ? 0 : 1);
  }
  wait(&st);
  check("read-your-own-writes inside the speculation", st == 0);
  check("the same write is not seen outside", holds("/sc_b", "B-old"));
  end_spec(id, 0);
  check("and is gone after abort", holds("/sc_b", "B-old"));
  unlink("/sc_b");
}

// Writing past what the shadow table can hold fails with -1, nothing
// reaches the disk, and what was written before still commits cleanly.
static void
test_shadow_exhaustion(void)
{
  char blk[1024];
  int id, ok, n_ok, mk_failed;

  unlink("/sc_big");
  id = open_spec();
  if (fork() == 0) {
    int n = 0;
    int fd;

    if (xjoin(id) < 0)
      exit(255);
    fd = open("/sc_big", O_CREATE | O_WRONLY);
    for (; n < 200; n++) {
      memset(blk, 'a' + n % 26, sizeof(blk));
      if (write(fd, blk, sizeof(blk)) != sizeof(blk))
        break;
    }
    close(fd);
    // The table is full: a mkdir (it needs a new data block) must fail too,
    // not silently lose its blocks.
    exit(n + 1000 * (mkdir("/sc_d") < 0) + 10000 * (n >= 200));
  }
  wait(&ok);
  n_ok = ok % 1000;
  mk_failed = (ok / 1000) % 10;
  printf("spectest: %d 1KB writes succeeded before the limit\n", n_ok);
  check("a write past the shadow limit returns -1", n_ok > 0 && ok < 10000);
  check("mkdir with a full shadow table returns -1", mk_failed == 1);
  check("nothing reached the disk before commit",
        !exists("/sc_big") && !exists("/sc_d"));
  // A refused write may have left an operation half done in the shadows, so
  // the speculation is FAILED: xcommit returns -1 and aborts it.
  check("xcommit of a speculation with a refused write returns -1",
        end_spec(id, 1) != 0);
  check("a failed speculation left nothing on the disk",
        !exists("/sc_big") && !exists("/sc_d"));
  check("a failed speculation is gone (xabort refuses)", end_spec(id, 0) != 0);
}

// A second xcommit or xabort, from another member or after the other
// one, fails and does not panic.
static void
test_commit_errors(void)
{
  int id, st, st2;

  // (Members cannot signal each other through a pipe: a write to a
  // reader outside the speculation is held.  Use ticks instead.)
  id = open_spec();
  if (fork() == 0) { // member one commits first
    if (xjoin(id) < 0)
      exit(2);
    pause(10);
    exit(xcommit() == 0 ? 0 : 1);
  }
  if (fork() == 0) { // member two tries after one has committed
    if (xjoin(id) < 0)
      exit(2);
    pause(40);
    exit(xcommit() == -1 && xabort() == -1 ? 0 : 1);
  }
  wait(&st);
  wait(&st2);
  check("a second member's xcommit/xabort after the commit fails",
        st == 0 && st2 == 0);

  id = open_spec();
  end_spec(id, 0);
  check("commit after abort fails", end_spec(id, 1) != 0);
  check("abort after abort fails", end_spec(id, 0) != 0);
}

// Modes used by scripts/tests/test_spec_crash.py.
static int
crash_modes(int argc, char **argv)
{
  if (argc == 2 && strcmp(argv[1], "probe") == 0) {
    probe_shadows();
    printf("spectest: shadow table: %s\n", have_shadows ? "yes" : "no");
    return 0;
  }
  // fill FILE BYTE COUNT [create]: write COUNT copies of BYTE at the start of
  // FILE.  Run inside a speculation (xrun) its console line is held too.
  if ((argc == 5 || argc == 6) && strcmp(argv[1], "fill") == 0) {
    char blk[1024];
    int left = atoi(argv[4]);
    int fd =
      open(argv[2], argc == 6 ? O_CREATE | O_WRONLY | O_TRUNC : O_WRONLY);

    if (fd < 0)
      return 1;
    memset(blk, argv[3][0], sizeof(blk));
    while (left > 0) {
      int k = left < (int)sizeof(blk) ? left : (int)sizeof(blk);

      if (write(fd, blk, k) != k)
        return 1;
      left -= k;
    }
    close(fd);
    printf("spectest: wrote %s\n", argv[2]);
    return 0;
  }
  // state FILE OLD NEW: is FILE all OLD bytes, all NEW bytes or mixed?
  if (argc == 5 && strcmp(argv[1], "state") == 0) {
    char blk[1024];
    int nold = 0, nnew = 0, other = 0, k;
    int fd = open(argv[2], O_RDONLY);

    if (fd < 0) {
      printf("spectest: state of %s: missing\n", argv[2]);
      return 0;
    }
    while ((k = read(fd, blk, sizeof(blk))) > 0)
      for (int i = 0; i < k; i++) {
        if (blk[i] == argv[3][0])
          nold++;
        else if (blk[i] == argv[4][0])
          nnew++;
        else
          other++;
      }
    close(fd);
    printf("spectest: state of %s: %s (%d old, %d new, %d other)\n", argv[2],
           other       ? "mixed"
           : nnew == 0 ? "old"
           : nold == 0 ? "new"
                       : "mixed",
           nold, nnew, other);
    return 0;
  }
  if (argc == 4 && strcmp(argv[1], "crashcommit") == 0) {
    if (xjoin(atoi(argv[2])) < 0)
      return 1;
    crashat(atoi(argv[3]));
    printf("spectest: commit returned %d\n", xcommit());
    crashat(0);
    return 0;
  }
  return -1;
}

int
main(int argc, char **argv)
{
  int r = crash_modes(argc, argv);

  if (r >= 0)
    exit(r);

  probe_shadows();
  printf("spectest: shadow table: %s\n", have_shadows ? "yes" : "no");

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
  test_commit_errors();

  // File effects need the shadow table (Heril's PR); until it is merged
  // these are reported as SKIP, not FAILED.
  if (have_shadows) {
    test_files(0);
    test_files(1);
    test_read_own_writes();
    test_isolation(0);
    test_isolation(1);
    test_read_takes_no_slot();
    test_shadow_exhaustion();
  } else {
    skip("files abort");
    skip("files commit");
    skip("read-your-own-writes");
    skip("isolation");
    skip("read takes no slot");
    skip("shadow exhaustion");
  }

  if (failures == 0)
    printf("spectest: all tests passed\n");
  else
    printf("spectest: %d test(s) FAILED\n", failures);
  exit(failures != 0);
}
