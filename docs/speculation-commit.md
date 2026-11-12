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

## What Heril's shadow table must provide

`kernel/speccommit.c` defines **weak** stand-ins, marked "TEMP until Heril's PR
merges", for every shadow function not on main, so this branch builds and the
non-shadow tests run now. His definitions override them at link time. They need
his exact names and signatures:

```
struct shadow *shadow_find(int spec, uint dev, uint blockno);
struct shadow *shadow_alloc(int spec, uint dev, uint blockno, struct buf *b);
void           shadow_free_all(int spec);
int            shadow_count(int spec);
int            shadow_get(int spec, int i, uint *dev, uint *blockno, char **data);   // NEW
```

1. **`shadow_get`** is the one function his table does not have: the i-th
   shadow (0-based) owned by `spec`, `-1` when there are fewer; `*data` points
   into the table and stays valid until `shadow_free_all(spec)`.
2. **A committing hook.** The speculative branch of `log_write()` must start
   with `if (!spec_can_shadow(p->spec)) return -1;` (defined in `spec.c`), so
   that members cannot add to or change shadows while the commit loop reads
   them.
3. **`MAXSHADOWBLOCKS <= SPECBLOCKS`** (checked by a `_Static_assert` when
   `MAXSHADOWBLOCKS` is defined).
4. **The buffer cache must not be modified in place.** With the current
   `bread`/`log_write` redirect a speculative write edits the *shared cache
   buffer* before `log_write` copies it into the shadow, and `bread` copies
   shadow data back into that shared buffer. An outside process then reads the
   speculative data, and `xabort` does not undo it. (Seen when merging: the
   `spectest` probe reports "shadow table: no" because the write is visible
   outside.) Speculative callers need a private buffer that is never in the
   cache list; for testing the merge I used a small pool of private buffers in
   `bio.c`.
5. **`log_write()`'s -1 is ignored by every caller** (`writei`, `balloc`,
   `iupdate`, ...), so a full shadow table loses writes silently and commits a
   half-written file. The simplest fix: a speculative `filewrite` first checks
   that at least `MAXOPBLOCKS` shadow slots are free and fails the write before
   touching any block. `test_shadow_exhaustion` in `spectest` checks the
   behaviour.
6. The shadow table is shared by all speculations (64 in total), not per
   speculation: one large speculation can starve another.

## Tests

* `user/spectest.c`: abort and commit file effects (unlink, overwrite, create),
  output arriving only after the files changed, read-your-own-writes,
  exhaustion, and the commit/abort error paths. File cases print
  `SKIP <case> (needs shadow table)` until the table is merged.
* `scripts/tests/test_spec_crash.py`: the crash-point table above.

Negative controls (run, not committed): replaying the output **before**
`end_op()` makes every crash round fail ("printed" for a commit that crashed);
splitting the commit into **two** transactions makes the file `mixed` at the
crash points between them.
