# Isolation bug: GDB investigation record

This record documents the shared-buffer isolation bug fixed by the private
buffer change (`fc8a0da`, `bio: speculative bread() returns a private buffer`).
The source trace below is reconstructed from that fix and the current code; it
is not a captured live target session. This checkout's GDB is built for x86-64,
and the installed QEMU is older than this project's supported version, so a
RISC-V guest could not be attached here.

## Environment checks

```text
$ gdb -q -batch -ex 'set architecture riscv:rv64' -ex 'show architecture'
Undefined item: "riscv:rv64".
The target architecture is set to "auto" (currently "i386").

$ qemu-system-riscv64 --version
QEMU emulator version 6.2.0

$ make test-spec
ERROR: Need qemu version >= 7.2
```

## Source trace

The failure was caused by the order of the normal xv6 file update:

```text
bread(dev, blockno) -> bp
caller edits bp->data
log_write(bp)
```

Redirecting only `log_write()` is too late: if speculative `bread()` returned
the shared cache buffer, the caller had already changed bytes visible to
outside readers before the write could be redirected.

The fix moves isolation to `bread()` in `kernel/bio.c`:

```text
speculative bread()
  -> pbread(spec, dev, blockno)
  -> private buffer with its own sleeplock
  -> copy from this speculation's shadow, or from bread_raw()
speculative log_write(private bp)
  -> shadow_put(...)
  -> no shared-cache buffer was modified
```

`brelse()` returns private buffers to the private pool. `bwrite()` rejects
them, and the log chooses the shadow path from `bp->spec`, so a speculative
caller cannot leak an edited shared cache block. The corresponding in-guest
checks are the shadow probe and `test_isolation()` in `user/spectest.c`.

## Live-session reproduction commands

With a RISC-V-capable GDB and supported QEMU, build with symbols and start
`make qemu-gdb`. In GDB, use:

```gdb
target remote :26000
break pbread
break log_write
break brelse
continue
```

Then run a speculative write and an outside read. At `pbread`, check that
`myproc()->spec` is nonzero and that the returned buffer has the same `spec`;
at `log_write`, check that it enters `shadow_put`; at `brelse`, check that the
private-buffer branch returns before touching the shared LRU list.
