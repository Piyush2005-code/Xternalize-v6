# Externalization barrier

An effect is *externalized* when someone other than the process that
produced it can see it. An agent that tries a command and then decides
whether to keep it needs that command's effects held back until it
decides. This change adds the barrier for the effects that leave a
process as a stream of bytes: console output, and pipe writes to
readers outside the speculation.

File writes are not held yet. A file written inside a speculation is
written for real. Isolating files needs shadow blocks, which come in the
next change and plug into the same `xcommit` and `xabort`.

## Using it

The shell stays outside the speculation and runs one command at a time
inside it. That way the shell's prompt is never held, which is what
lets the qemu-bridge (and the agent behind it) drive a speculation.

```
admin# xbegin
speculation 1
admin# xrun 1 echo hello
admin# xrun 1 echo world
admin# xcommit 1
hello
world
speculation 1 committed
admin# xbegin
speculation 2
admin# xrun 2 echo secret
admin# xabort 2
speculation 2 aborted
```

| Command | Does |
|---|---|
| `xbegin` | opens a speculation and prints its id |
| `xrun <id> <cmd> [args]` | runs one command inside speculation `<id>` |
| `xcommit <id>` | releases everything it held, in order, then ends it |
| `xabort <id>` | drops everything it held, then ends it |

## System calls

| Call | Does |
|---|---|
| `int xbegin(void)` | opens a speculation, puts the caller in it, returns its id |
| `int xjoin(int id)` | puts the caller into open speculation `id` |
| `int xcommit(void)` | ends the caller's speculation and releases its held output |
| `int xabort(void)` | ends the caller's speculation and drops its held output |

All return -1 on failure. `fork()` copies `p->spec`, so every process a
speculative process creates is in the same speculation. `xcommit` and
`xabort` take every member out of the speculation, not just the caller.
`xjoin` is what lets `xrun` put a command inside a speculation that its
shell is not part of. A speculation stays open until someone commits or
aborts it, even after all its processes have exited. All four calls are
open to normal users.

## What is held, and when

| Write | Held? | Why |
|---|---|---|
| console, from a speculative process | yes | the person at the console is outside |
| pipe created outside the writer's speculation | yes | its reader is outside |
| pipe created inside the writer's speculation | no | the reader is inside; holding it would make every pipeline hang |
| kernel `printk` | no | not a process effect |
| files | not yet | shadow blocks, next change |

A pipe remembers the speculation it was created in (`pi->spec`). The
rule is therefore "is my reader outside my speculation", not "am I
speculative".

Held pipe data counts as a writer that is still open. If the writer
exits before the commit, the reader keeps waiting instead of seeing
end-of-file. It gets the bytes at `xcommit`, or end-of-file at
`xabort`, and the pipe is not freed while data is held for it.

## Where the held bytes live

`kernel/spec.c` keeps a fixed table of `NSPEC` speculations, all in
kernel memory. Each speculation has a `HELDBYTES` byte buffer and up to
`NHELD` records. A record says where a run of bytes was going (the
console or a pipe) and where it sits in the buffer. Consecutive writes
to the same place extend the last record, so a program that prints one
character at a time uses one record. `xcommit` replays the records in
order; `xabort` forgets them.

The limits live in `kernel/param.h`:

| Constant | Default | Meaning |
|---|---|---|
| `NSPEC` | 4 | speculations open at once |
| `NHELD` | 64 | held records per speculation |
| `HELDBYTES` | 4096 | held bytes per speculation |

When a speculation runs out of room, the write fails with -1. Writing
for real instead would externalize, which is the one thing the barrier
must never do.

## Code

| File | Change |
|---|---|
| `kernel/proc.h`, `kernel/proc.c` | `p->spec`, copied by `fork()`; `proc_leave_spec()` |
| `kernel/spec.c` | speculation table, held output, begin/join/end |
| `kernel/console.c` | `consolewrite()` holds speculative output |
| `kernel/pipe.c` | `pi->spec`, `pi->held`, holding in `pipewrite()` |
| `kernel/sysproc.c`, `syscall.c`, `syscall.h` | the four system calls |
| `user/xbegin.c`, `xrun.c`, `xcommit.c`, `xabort.c` | commands |
| `user/spectest.c` | in-guest tests |
| `scripts/tests/test_spec_e2e.py` | end-to-end test through the qemu-bridge |

## Testing

```
make test-spec          # through the qemu-bridge, CPUS=1 and CPUS=3
```

or, inside xv6, `spectest`. CI runs `make test-spec` on every push.

## Limitations

- File writes, creates and deletes are not held (next change).
- Nested speculations are not supported: `xbegin` inside one fails.
- A write that finds the buffer full fails. It is not partly held.
- If the speculation is ending (inside `xcommit` or `xabort`) when a
  member writes, that write fails.
- `xcommit` may block while delivering to a pipe whose buffer is full,
  until the reader reads.
- A process that is still running when its speculation commits simply
  continues outside it. Later writes go through directly.
- A process forked at the same moment its speculation ends can be left
  holding the old id. Its writes then fail; they are never written for
  real.
- Held output takes kernel memory even after its processes have exited,
  until someone commits or aborts. A forgotten speculation occupies one
  of the `NSPEC` slots.
