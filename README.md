# xv6-riscv — Speculative Processes, Permissions & an AI Agent

> A multi-user [xv6-riscv](https://github.com/mit-pdos/xv6-riscv) with password login and file permissions, an **externalization barrier for speculative processes** (`xbegin` / `xrun` / `xcommit` / `xabort`), crash-atomic speculative file commits, and a host-side **AI agent** that drives the live OS over the serial console.

![C](https://img.shields.io/badge/C-A8B9CC?logo=c&logoColor=black)
![RISC-V](https://img.shields.io/badge/RISC--V-283272?logo=riscv&logoColor=white)
![QEMU](https://img.shields.io/badge/QEMU-FF6600?logo=qemu&logoColor=white)
![Python](https://img.shields.io/badge/Python-3776AB?logo=python&logoColor=white)
![GNU Make](https://img.shields.io/badge/GNU%20Make-A42E2B?logo=gnu&logoColor=white)
![GitHub Actions](https://img.shields.io/badge/CI-GitHub%20Actions-2088FF?logo=githubactions&logoColor=white)
![MCP](https://img.shields.io/badge/MCP-Model%20Context%20Protocol-111)
![License: MIT](https://img.shields.io/badge/License-MIT-success)

---

## Overview

This is MIT's teaching operating system **xv6-riscv**, extended for a university Operating Systems course. The centerpiece (**Project 5**) is an *externalization barrier*: a way to run a group of processes *speculatively* — holding back everything they would show the outside world (console output, pipe writes, file changes) until the group explicitly **commits**, or throwing it all away on **abort**. This lets an agent try a command and then decide whether to keep its effects.

Around that core, the OS was made multi-user (login, privilege levels, per-file permissions, safe path resolution), and a small AI agent on the host drives the guest through the serial console to demonstrate speculation in action.

## Features

| Layer | What it adds |
|---|---|
| **Login & privilege** | Password login (`admin` / `user`), `uid`-based privilege, `setuid`/`getuid`, and a fail-closed per-syscall privilege table. |
| **File permissions** | Per-inode owner + 6-bit `rwx` mode, enforced in `open`/`create`/`unlink`/`link`/`exec`; admin bypasses, users are checked. |
| **Path resolution** | A user-space `execvp()` so commands run from any working directory, searching `/` first (untrusted-cwd safe). |
| **Externalization barrier** *(Project 5)* | `xbegin` / `xjoin` / `xcommit` / `xabort`; a speculation's console and pipe output is held and released on commit or dropped on abort. |
| **Shadow table + crash-atomic commit** *(Project 5)* | A speculation's file writes are isolated in an in-memory shadow table and applied in a single, crash-atomic log transaction. |
| **Commit quiesce** *(Project 5)* | `xcommit`/`xabort` wait for in-flight member operations to finish, so a half-done operation is never committed. |
| **AI agent** | An LLM agent (OpenRouter) that controls the live OS via MCP tools over a TCP bridge to the QEMU serial console. |
| **CI** | GitHub Actions: format gate, kernel + `fs.img` build, `usertests`, and the crash-recovery test on every push. |

## Tech stack

- **Kernel & user programs:** C (freestanding, `-ffreestanding`, no libc), RISC-V (`rv64gc`)
- **Toolchain:** `riscv64` GCC cross-compiler, GNU Make, `clang-format`
- **Emulation:** QEMU (`qemu-system-riscv64`, the `virt` machine)
- **Agent:** Python 3, the Model Context Protocol (JSON-RPC over stdio), OpenRouter; a TCP serial bridge
- **Tests & CI:** Python harnesses, GitHub Actions

## Repository layout

```
kernel/      the xv6 kernel  (spec.c, shadow.c, speccommit.c are the Project-5 core)
user/        user programs   (login, whoami, x{begin,run,commit,abort}, spectest, ...)
mkfs/        host tool that builds fs.img
agent/       host AI agent    (qemu_bridge.py, mcp_server.py, mcp_client.py, llm_agent.py)
scripts/     run_agent.sh and the Python end-to-end test harnesses
docs/        design write-ups (externalization, speculation-commit, permissions, path)
.github/     CI workflows
```

## Quick start

**Prerequisites:** a `riscv64` GCC toolchain and QEMU (`qemu-system-riscv64`). On Debian/Ubuntu: `sudo apt install gcc-riscv64-linux-gnu qemu-system-misc`.

```bash
make qemu            # build and boot xv6
# at the prompt:  Login as (admin/user):
#   admin  (password: admin)   → full privileges
#   user   (no password)       → unprivileged; file permissions apply
```

Run `make clean` before rebuilding after a pull — the permission and shadow-table work changed the on-disk layout. Quit QEMU with `Ctrl-A X`.

Try speculation by hand:
```
admin# xbegin                 # → speculation 1
admin# xrun 1 echo hello      # runs inside speculation 1 (output held)
admin# xcommit 1              # releases: prints "hello"   (or: xabort 1 discards it)
```

## The AI agent

The agent runs on the **host** and talks to xv6 through two TCP hops: QEMU exposes the serial console on `:5555`, `qemu_bridge.py` logs in and relays commands on `:4444`, and `mcp_server.py` exposes them as MCP tools to the LLM. **The bridge logs in as the unprivileged `user` by default**, so the agent operates with file permissions in force (admin would bypass them).

```bash
pip install -r agent/requirements.txt
export OPENROUTER_API_KEY=sk-or-...        # or put it in agent/.env
scripts/run_agent.sh                       # builds, starts QEMU + bridge + agent REPL
```

Override the identity with `XV6_USER=admin XV6_PASSWORD=admin` for an admin session. See [`agent/README.md`](agent/README.md) for the manual three-terminal setup and the full environment-variable list.

## Testing

```bash
spectest                 # (inside xv6) in-guest speculation tests
./test-xv6.py usertests  # the classic xv6 regression suite
make test-spec           # externalization end-to-end via the bridge (CPUS=1 and 3)
make test-crash          # crash-atomicity of xcommit at every crash point
make test-path           # path-resolution end-to-end
make test-perm           # permission end-to-end
make fmt                 # clang-format; CI requires a clean diff
```

## Documentation

In-depth design notes live in [`docs/`](docs/):

- [`docs/externalization.md`](docs/externalization.md) — the console/pipe output barrier
- [`docs/speculation-commit.md`](docs/speculation-commit.md) — shadow table, one-transaction commit, quiesce
- [`docs/path-resolution.md`](docs/path-resolution.md) — `execvp` and the search path
- [`docs/changes.md`](docs/changes.md) — file permissions and privilege levels

## Project status

Project 5 is substantially complete and tested: the four speculation calls, `fork` inheritance, session ownership, console + pipe holding, shadow-based file isolation, crash-atomic commit, and commit quiesce all work and are covered by `spectest`, `make test-spec` and `make test-crash`. The main remaining piece is a **conflict rule** (when two speculations write the same block, the later commit currently wins) — documented, and built on top of the quiesce work.

## Credits

Built on **[xv6-riscv](https://github.com/mit-pdos/xv6-riscv)** by MIT PDOS — a re-implementation of Unix v6 for RISC-V. Course project for Operating Systems at IIT Jodhpur, by Piyush Singh Bhati, Brijesh Thakkar, Chaitanya Singh and Heril Mistry; per-commit authorship is preserved in the git history.

## License & attribution

This project is a derivative of xv6-riscv and is distributed under the **MIT License** — see [`LICENSE`](LICENSE). The original copyright is retained in full:

> Copyright © 2006–2024 Frans Kaashoek, Robert Morris, Russ Cox, and the Massachusetts Institute of Technology.

Our additions (the modules listed above) are released under the same MIT License. The MIT License permits using, modifying and redistributing the software — **including re-hosting it as your own repository** — provided the copyright notice and the license text are kept, which this repository does. The full upstream git history is retained, so every commit's original author is preserved. We do not claim authorship of xv6 itself; this is coursework built on top of it.

*This section is general information, not legal advice.*
