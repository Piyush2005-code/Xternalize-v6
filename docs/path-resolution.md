# Command path resolution (`execvp`)

## What changed

Before, `sh` called `exec(name, argv)`. The kernel resolves `name` relative to
the process's cwd, so after `cd /some/dir`, `ls` failed with `exec ls failed`
(the binary is `/ls`). All `UPROGS` binaries are installed in the root
directory of `fs.img` by `mkfs` (`_` prefix stripped), so only `/ls`, `/cat`,
... exist.

Now there is a user-space library function, and the shell uses it:

| File | Change |
|------|--------|
| `user/ulib.c`, `user/user.h` | new `int execvp(char *file, char **argv)` |
| `user/sh.c` | `EXECCMD` case calls `execvp()` instead of `exec()` |
| `user/pathtest.c` | in-xv6 test (added to `UPROGS`) |
| `scripts/test_path_e2e.py`, `make test-path` | host-side end-to-end test |
| `Makefile`, `.gitignore` | `pathtest` in `UPROGS`; generated `user/uprogs.h` |

There are **no kernel, `fs.h` or `mkfs` changes**, so `make clean` is **not**
needed to pick this up (an ordinary `make qemu` rebuilds what changed).

## `execvp` semantics

```c
int execvp(char *file, char **argv);
```

1. `file == NULL` or `file[0] == '\0'`: return `-1`.
2. `file` contains a `/` anywhere (`/ls`, `./prog`, `a/b/prog`): `exec(file, argv)`
   is called exactly once. No search, so no fallback to `/`.
3. Otherwise each prefix in `searchpath[]` (in `user/ulib.c`) is tried in order:
   1. `""`: the name as given, relative to the cwd (the old xv6 behaviour)
   2. `"/"`: `"/" + name`
4. Each candidate is built in a `MAXPATH` (128) buffer. If prefix + name + NUL
   does not fit, that candidate is **skipped** (never truncated, never
   overflows). A name of `MAXPATH-1` characters therefore tries only the
   as-given candidate and then fails.
5. `argv` is passed through unchanged (`argv[0]` stays as typed).
6. `exec` only returns on failure; if every candidate fails, `execvp` returns `-1`.
   A candidate that exists but is not a valid ELF just fails, and the search
   continues with the next one.

To add a directory (for example `/bin`) change one line:
`static char *searchpath[] = { "", "/", "/bin/", 0 };` (prefixes must end in `/`
except the empty one).

## Using it in your own programs

```c
char *args[] = { "ls", "-l", 0 };   // argv[0] is the name, list ends with 0
if (fork() == 0) {
  execvp("ls", args);               // works from any cwd
  fprintf(2, "exec ls failed\n");   // only reached if it failed
  exit(1);
}
wait(0);
```

Use `execvp` for bare command names. Keep plain `exec` when you want the
exact kernel semantics (tests of `exec` itself, or an explicit path).

Call sites reviewed when this was added:

| Site | Decision |
|------|----------|
| `user/sh.c` | **changed** to `execvp` |
| `user/init.c` `exec("sh")` | left: runs once at boot with cwd `/`, no benefit |
| `user/usertests.c` (all `exec`) | left: these test `exec` itself |
| `user/grind.c` | left: already uses paths containing `/` |

## Running the tests

In xv6 (from any directory):

    $ pathtest
    ...
    pathtest: ALL OK

It creates `/pt/a/b`, `cd`s there, forks per case and prints `PASS`/`FAIL`.
It runs **every program in `UPROGS`** by bare name (the list is generated from
the Makefile into `user/uprogs.h`, so programs your teammates add are covered
automatically). Output goes to a scratch file; a watchdog kills any program
still running after ~3 s (counted as resolved: it started). Skipped:
`init` (respawns a shell forever) and `grind` (never exits, litters the fs).
`usertests` is run as `usertests -x` and `pathtest` as `pathtest --probe` so
they exit immediately. `/pt` is removed at exit, including after failures.

On the host:

    make test-path                              # CPUS=1 then CPUS=3, incl. usertests -q
    python3 scripts/test_path_e2e.py --cpus 1 --skip-usertests   # quick check

Standard-library Python 3 only. It boots QEMU, types into the real shell, prints
a PASS/FAIL table and exits non-zero on any failure. Raw QEMU logs are saved to
`scripts/logs/path_e2e_<cpus>.log`. `fs.img` is backed up first and restored after
every boot. Per-command and overall timeouts (`--timeout`, default 2400 s) prevent hangs.

## Known limits

- The search path is fixed in the source; there is no `PATH` environment
  variable (xv6 has no environment).
- cwd comes first, so a real executable in the cwd **shadows** the one in `/`.
  A non-executable file with the same name does not (the search falls through).
- A name containing `/` is never searched.
- Names are limited by `MAXPATH`; the shell's `MAXARGS` (10) still applies.
- Only the shell and callers of `execvp` get this; a raw `exec("ls")` is unchanged.
