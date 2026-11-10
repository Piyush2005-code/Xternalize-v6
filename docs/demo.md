# Official Speculative Demo: `rm a` and `echo hi > b`

This document details the official demo script implementing Task 5:
> **Official demo script**: Speculative `rm a` plus `echo hi > b`: abort leaves no trace, commit makes both appear at once.

---

## 1. What the Demo Demonstrates

Speculative execution introduces transactional atomicity and isolation to xv6 filesystem operations and externalized byte streams:

1. **Isolation before ending the speculation**:
   While a speculation is open, changes made by processes running inside it (in private shadow buffers) are completely invisible to any process outside the speculation.
2. **Abort leaves no trace**:
   When `xabort` is called, all held output and shadow modifications are discarded.
   - File `a` was scheduled for deletion (`rm a`), but after `xabort` it remains completely intact with its original contents.
   - File `b` was created and written (`echo hi > b`), but after `xabort` it does not exist on disk at all.
3. **Commit applies all changes atomically at once**:
   When `xcommit` is called, all shadow block changes are committed in a single log transaction, and held output is released.
   - File `a` is deleted.
   - File `b` is created with `"hi"`.
   - Both changes become visible to the outside system simultaneously at the exact commit point.

---

## 2. Interactive xv6 Shell Usage

Inside xv6, the demo script `demoscript` is installed on the filesystem image:
```sh
$ cat demoscript
rm a
echo hi > b
```

### Abort Run (Leaves No Trace)
```
admin# echo hello > a
admin# rm b
admin# xbegin
speculation 1
admin# xrun 1 sh demoscript
admin# cat a
hello
admin# cat b
cat: cannot open b
admin# xabort 1
speculation 1 aborted
admin# cat a
hello
admin# cat b
cat: cannot open b
```
*Result*: `a` remains `hello`; `b` was never created.

---

### Commit Run (Both Appear at Once)
```
admin# echo hello > a
admin# rm b
admin# xbegin
speculation 2
admin# xrun 2 sh demoscript
admin# cat a
hello
admin# cat b
cat: cannot open b
admin# xcommit 2
speculation 2 committed
admin# cat a
cat: cannot open a
admin# cat b
hi
```
*Result*: `a` is removed, and `b` contains `"hi"`. Both changes appeared atomically.

---

## 3. In-Guest Verification Utility (`demospec`)

A standalone C program `user/demospec.c` is provided in `UPROGS` and compiled into `/demospec`:
```sh
admin# demospec
```
Options:
- `demospec` (runs both abort and commit checks)
- `demospec abort` (runs only the abort check)
- `demospec commit` (runs only the commit check)

It programmatically asserts every pre-condition and post-condition via system calls (`xbegin`, `xjoin`, `xabort`, `xcommit`, `unlink`, `open`, `read`, `write`) and prints verified `PASS` status.

---

## 4. Host-Side Automated Runner (`scripts/demo_spec.py`)

From the host terminal, run:
```bash
./scripts/demo_spec.py [--cpus 1]
# or:
./scripts/demo.sh
```

Features:
- Boots QEMU automatically and logs in as `admin`.
- Runs Phase 1 (Abort demo), Phase 2 (Commit demo), and Phase 3 (In-guest C verifier).
- Beautifully colorized step-by-step console reporting.
- Backs up and restores `fs.img` cleanly.
- Saves full raw logs to `scripts/logs/demo_spec_<cpus>.log`.

### Automated E2E Test Suite
The automated test is located at `scripts/tests/test_spec_demo.py`:
```bash
./scripts/tests/test_spec_demo.py --cpus 1 3
```
Exits `0` on success and `1` on failure, integrating directly into CI.
