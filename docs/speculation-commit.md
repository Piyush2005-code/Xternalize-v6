# Committing a speculation: one transaction, output after `end_op()`

`xcommit` makes a speculation's file changes durable and then lets its held
output out. `xabort` throws both away. The output barrier itself (held console
and pipe output) is in `docs/externalization.md`; the shadow table that keeps
the file changes in memory is Heril's (`kernel/shadow.c`). This document is the
bridge: what `xcommit` does with the shadows and in which order.

Files: `kernel/speccommit.c` (new), a small call site in `kernel/spec.c`
(`spec_end`), `begin_op_n`/`end_op_n` and three crash points in `kernel/log.c`,
the `crashat` syscall.

## The protocol

`spec_end(commit)` in `kernel/spec.c`, for the caller's speculation `id`:

1. **Mark it ending** under `spec_lock`, then drop the lock. From now on
   `spec_can_shadow(id)` is false and members' new shadow writes must fail with
   -1. Unknown id, double commit, commit after abort and commit while already
   ending all return -1 here and never panic.
2. **Apply the shadows**, `spec_commit_files(id)`:
   1. the caller is made non-speculative for the loop, so `bread()` and
      `log_write()` reach the real blocks and not its own shadows;
   2. `begin_op_n(n)` reserves room for all `n` shadow blocks;
   3. for each shadow: `bread()` the real block, copy the shadow data in,
      `log_write()`, `brelse()`;
   4. `end_op_n(n)` commits the transaction.
3. **Only after `end_op_n()` has returned** the members leave the speculation
   (`proc_leave_spec`), the shadows are freed and the held output is replayed
   in its original order (`release_held`, Piyush's code, not duplicated).
4. Free the slot.

`xabort` skips step 2: it frees the shadows and drops the held output. So does
the session-reclaim path (`discard_slot`).

No spinlock is held across `bread`, `begin_op_n`, `end_op_n` or the pipe replay.

### Why this is crash-atomic

All shadows go into **one** log transaction. A transaction commits when
`write_head()` writes the log header: before that the on-disk files are the old
ones and recovery ignores the log, after it recovery re-installs every logged
block. There is no state in between, so after a crash the files are wholly old
or wholly new, never half applied. Splitting the commit over two transactions
would break exactly that (negative control 2 below). That is why `end_op()` is
never called early and why the log had to grow.

### Why the output goes out after `end_op()`

The handout: *flush the held output only after `end_op()` returns, or a crash in
between prints something that then did not happen.* If the output came out
before the commit point and the machine crashed, the user would have seen
"saved" for a file that recovery then restores to its old contents. After
`end_op()` the changes are durable, so anything printed is true. The converse is
harmless: a crash after `end_op()` but before the replay loses output for
changes that did happen. That output was never externalized, so losing it
breaks no promise.

## The log size: option (a)

`MAXOPBLOCKS` is 10 and `LOGBLOCKS` was 30 (`NBUF` 30), so a speculation with
more than about 10 shadow blocks could not commit atomically. Chosen:
**raise the log and reserve exactly what the commit needs.**

| | before | after |
|---|---|---|
| `MAXOPBLOCKS` | 10 | 10 (unchanged: `filewrite` batches by it) |
| `SPECBLOCKS` | - | 64 (= `MAXSHADOWBLOCKS`) |
| `LOGBLOCKS` | 30 | 84 = `SPECBLOCKS + 2 * MAXOPBLOCKS` |
| `NBUF` | 30 | 94 = `LOGBLOCKS + MAXOPBLOCKS` |
| log blocks on disk (`mkfs`) | 31 | 85 |
| metadata blocks | 48 | 102 |
| blocks free after `mkfs` (of 2000) | about 514 | about 490 |

* 64 so that all of `MAXSHADOWBLOCKS` fits, plus room for two ordinary
  operations (10 each) next to a full commit.
* `NBUF` must outgrow `LOGBLOCKS`: every `log_write()`n block stays pinned in the
  buffer cache until installed, and `write_log` needs two more buffers.
* The log header (`4 + 4 * LOGBLOCKS` = 340 bytes) still fits one block.
* `begin_op_n(n)` replaces `outstanding * MAXOPBLOCKS` by a `log.reserved`
  counter. `begin_op()` is `begin_op_n(MAXOPBLOCKS)`, so ordinary behaviour is
  unchanged. A waiting large reservation holds back ordinary calls, so a commit
  cannot be starved by a stream of small operations.

**This changes the disk layout. Run `make clean` after pulling.**

## Crash points

`crashat(n)` (admin only, `0` turns it off) makes the n-th crash point of the
next commit `panic("crashat: injected crash")`. With `k` shadow blocks:

| n | where | files after reboot |
|---|---|---|
| 1 .. k | after staging each shadow block (`log_write`) | old |
| k+1 | `write_log` done, just before `write_head` | old |
| k+2 | just after `write_head` (the commit point) | new, recovery runs |
| k+3 .. 2k+2 | after each block installed in `install_trans` | new, recovery runs |
| 2k+3 | after `end_op()` returned, before any output | new, no recovery needed |
| 2k+4 | no crash: the commit finishes and the output is printed | new |

Recovery after a crash at k+2 .. 2k+2 re-installs from the log (the boot prints
`recovering tail ...`). Output is never printed for a commit that crashed.

`scripts/tests/test_spec_crash.py` (`make test-crash`) runs every point for
CPUS=1 and 3 with `k = 4` (a 3000-byte file: 3 data blocks + the inode) and
prints the table. Observed, CPUS=1 and 3 alike: points 1-5 old, 6-11 new,
12 finishes. Points 6-10 show `recovering`.

## Limits

* **No conflict rule.** Two speculations that write the same block each commit
  their whole shadow; the later commit silently wins. The handout says to drop
  this first when behind, and we did.
* **No rollback of process memory.** `xabort` discards file and output effects
  only; a process keeps its variables, its open file descriptors, its pid.
* **Held output lost on a crash before `end_op()`** is correct, as above.
* **One crash-injection counter.** `crashat` is global, not per process; it is
  armed only inside `spec_commit_files`, so unrelated commits do not consume
  it, but another process's `end_op` that commits at the same moment could. The
  test runs on a quiet machine.
* A speculation can hold at most `SPECBLOCKS` (64) shadow blocks; more is
  refused before anything is written (`spec_commit_files` returns -1 and the
  speculation is dropped).

## The shadow layer (isolation, failure, committing)

The shadow table (`kernel/shadow.c`) plus these changes make the file side of a
speculation correct. Each point lists the invariant and where it is enforced.

**1. Isolation (`bio.c`).** A speculative process never gets, and so never
modifies, a buffer of the shared cache; nobody else ever sees a speculative
change. xv6 code does `bread(); edit bp->data; log_write(); brelse()`, so
redirecting in `log_write()` is too late. `bread()` by a speculative process
returns a *private* `struct buf` (pool of 64, `buf.spec != 0`), keyed by
(spec, dev, blockno) with its own sleeplock - so members of one speculation
still exclude each other the way cache buffers do - and filled from the
speculation's shadow of the block if there is one, else from the real block.
`brelse()` never puts it on the cache list; `bwrite()` panics on it.
`log_write()` decides by the buffer, not by who is running: a private buffer's
data goes to the shadow (`shadow_put`), never into the real log. (A speculative
process can run the log's own commit from `end_op()`, so the log and
`spec_commit_files()` read with `bread_raw()`, which is never redirected.)

*Read-only accesses burn no slot:* a private buffer exists only while held and
a shadow slot is created only by `log_write()`, so a block that was only read
leaves nothing behind (test: after reading 40 blocks, the same 59 writes still
fit).

**1b. In-memory inodes (`fs.c`).** The inode table is a second shared cache:
`writei` sets `ip->size`, `bmap` sets `ip->addrs[]`, before `iupdate()`. Each
cached inode therefore carries `view` (the speculation whose view of the disk
it was read in, 0 = real) and `ilock()` re-reads it when the caller's view
differs. `iput()` only frees by in-core fields read in the caller's view.
`fileread`/`filewrite` return -1 for an open file whose inode does not exist in
the caller's view (created in an aborted speculation).

**2. No silent loss (`fs.c`, `sysfile.c`, `spec.c`).** `log_write()` returns -1
when the table is full, and that is now passed up through `bzero`, `balloc`,
`bfree`, `ialloc`, `iupdate`, `ifree`, `itrunc`, `bmap` and `writei`, so
`write`, `open(O_TRUNC)`, `link`, `unlink`, `mkdir` and `mknod` return -1. Where
a function cannot return an error (`iput`, `create`'s `iupdate`) the per-process
flag `p->fsfail` (set by `log_write`, cleared by `begin_op`, read by
`op_failed()`) makes the syscall fail anyway. A refused write may leave an
operation half done in the shadows, so the speculation is marked **FAILED**
(`spec.c`, `failed`): `spec_commit_files()` refuses it, `spec_end()` drops it
as for `xabort`, and `xcommit` returns -1. A FAILED speculation can only be
aborted; reads inside it may show the half-done state.

**3. `shadow_get(spec, i, &dev, &blockno, &data)`** is the iterator the commit
walks; `shadow_count(spec)` says how many to expect.

**4. Committing check.** `shadow_alloc` and `shadow_put` ask `spec_can_shadow()`
(spec.c's existing `ending` flag) with the table lock held; the commit reads the
table after `ending` is set. A member write therefore either lands before the
commit counts the shadows or fails. Lock order: `stable.lock`, then `spec_lock`.

Remaining limits of this layer:

* The table is shared by all speculations (64 blocks in total), not per
  speculation: one large speculation can starve another.
* A member in the middle of a multi-block syscall while another member commits
  can have the first half committed and the second half refused (the commit
  does not wait for in-flight member operations).
* A file descriptor to a file created inside an aborted speculation stays open
  but every use returns -1; `xabort` does not close descriptors.
* Whole-block shadows: an outside process that changes a block (bitmap, inode
  block, directory block) while a speculation holds a shadow of it is
  overwritten at commit (the conflict rule, above).

## Cost

`scripts/bench_spec_cost.py` (CPUS=1, ms per KB written, uptime tick = 0.1 s,
measured on `help/shadow-fixes`, i.e. with the real shadow table):

| KB | plain | inside a speculation | xbegin+write+xcommit | xbegin+write+xabort |
|---|---|---|---|---|
| 1 | 18.8 | 4.30 | 23.4 | 5.08 |
| 4 | 17.2 | 1.27 | 10.9 | 1.47 |
| 16 | 17.2 | 0.54 | 7.4 | 0.64 |
| 48 | 17.7 | 0.46 | 8.9 | 0.52 |
| 56 | 17.0 | 0.45 | 7.6 | 0.50 |
| 64 | 15.6 | **write fails** | **write fails** | **write fails** |

Plain writes pay one disk transaction per 3 KB. Inside a speculation they cost
memory only; commit pays one transaction for everything, which is why its cost
per KB falls as the speculation grows. The cliff is at 64 KB: the shadow table (64 blocks) holds 56 KB of data plus the inode, indirect, bitmap and directory blocks, and the next write returns -1. (Before private buffers the cliff was at 56 KB; the plain column is noisier on this machine than in the first measurement.) The CSV and plot are in `scripts/logs/`.

## Tests

* `user/spectest.c`: abort and commit file effects (unlink, overwrite, create),
  output arriving only after the files changed, read-your-own-writes,
  exhaustion (write -1, mkdir -1, xcommit -1), isolation (old size, data and
  directory outside; create+unlink inside; abort leaves the cache intact),
  read-takes-no-slot, and the commit/abort error paths. (With no shadow
  table the file cases print `SKIP`.)
* `scripts/tests/test_spec_crash.py`: the crash-point table above.
* `scripts/bench_spec_cost.py`: write cost inside and outside a speculation.

Negative controls (run, not committed): replaying the output **before**
`end_op()` makes every crash round fail ("printed" for a commit that crashed);
splitting the commit into **two** transactions makes the file `mixed` at the
crash points between them.
