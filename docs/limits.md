# System Limitations of xv6 Speculative Execution

This document details the architectural boundaries, design trade-offs, and operational limits of the speculative execution and externalization barrier implementation in xv6.

---

## 1. No Memory Rollback

* **Scope**: Speculative rollback applies exclusively to **filesystem state** (via in-memory shadow blocks) and **externalized byte streams** (held console and cross-speculation pipe output).
* **Behavior**:
  * When `xabort()` is invoked, shadow blocks are freed and held output buffers are cleared.
  * **Process memory is not reverted**: user heap allocations (`sbrk`/`malloc`), global variables, stack variables, CPU registers, process IDs (`pid`), and open file descriptor tables remain in their post-speculation state.
* **Rationale & Trade-off**:
  * Implementing full memory rollback requires page-level copy-on-write snapshots, shadow page tables, and memory undo-logging, which introduce substantial memory overhead and TLB shootdown penalties.
  * In the xv6 agent workflow, speculative tasks are executed by separate member processes (e.g., child processes invoked via `xrun <id> <command>`). When a speculative process terminates or is discarded on abort, its virtual address space is freed by `kexit()`, naturally recycling memory without requiring kernel memory rollback.

---

## 2. Conflict-Rule Scope

* **Granularity**: Block-level (`BSIZE = 1024` bytes), keyed strictly by `(dev, blockno)`.
* **Conflict Definition**:
  * A conflict arises when a block modified within an active speculation is concurrently written by an outside non-speculative process or another concurrent speculation before `xcommit()` completes.
* **Resolution & Commit Behavior**:
  * **Base / Unsynchronized Model**: Transactions commit their shadows sequentially in a single `end_op()` transaction; the later commit overwrites shared blocks.
  * **Versioned Conflict Detection**: Per-block version counters are checked during commit. If an external write modified any block touched by the speculation since `xbegin()`, `xcommit()` rejects the transaction and returns `-2`.
* **Out of Scope**:
  * **No Inode-Level Disambiguation**: Two independent files residing on the same disk block (e.g., sharing an inode block `IPB` or directory entry block) register a block collision despite operating on logically distinct files.
  * **No Semantic Merging**: There is no three-way merge or record-level delta application. Any write collision causes full transaction abort.

---

## 3. The 56 KB / 64 KB Cliff

* **Root Cause**: Fixed shadow table capacity (`MAXSHADOWBLOCKS = 64` in `kernel/param.h`).
* **Block Accounting**:
  * Every distinct block written by a speculation allocates one `struct shadow` slot in the in-memory shadow table.
  * File writes require both data blocks and filesystem metadata blocks:
    * File data blocks (up to 12 direct blocks + indirect block addressing).
    * 1 indirect address block (for files exceeding `NDIRECT = 12` blocks).
    * 1 inode block (for file size and block pointer updates).
* **The Cliff Threshold**:
  * **At 56 KB**:
    $$\text{Data blocks: } 56 + \text{Indirect block: } 1 + \text{Inode block: } 1 = 58 \text{ blocks} \le 64$$
    *Result*: All 58 blocks fit in the shadow table; writes and commit succeed 100%.
  * **At 64 KB**:
    $$\text{Data blocks: } 64 + \text{Indirect block: } 1 + \text{Inode block: } 1 = 66 \text{ blocks} > 64$$
    *Result*: The 65th block allocation exhausts `MAXSHADOWBLOCKS`.
* **Failure Semantics**:
  * When the shadow table fills, `log_write()` returns `-1`, setting `p->fsfail` and marking the speculation state as `FAILED`.
  * Subsequent writes fail immediately.
  * `xcommit()` detects `FAILED` status, refuses the commit, and returns `-1`. The speculation can only be aborted.
  * This creates a steep performance and functional cliff where payloads up to 56 KB succeed and 64 KB payloads fail completely.

---

## 4. `NSPEC` Slot Limit

* **Constraint**: `NSPEC = 4` (`kernel/param.h`).
* **Behavior**:
  * At most 4 distinct speculations can be open system-wide concurrently.
  * If all 4 slots are occupied, any subsequent `xbegin()` system call returns `-1`.
* **Ownership & Session Lifecycle**:
  * Speculations are owned by the login session (`p->sid`), not by individual transient worker processes.
  * **Reclamation Paths**:
    1. Explicit termination: Calling `xcommit()` or `xabort()` ends the speculation and frees its slot.
    2. Clean session logout: When the session leader shell exits, `kexit()` automatically discards any uncommitted speculations owned by that session ID.
    3. Orphan sweep: When an unhandled crash or kill occurs, the next `xbegin()` scans the table and reclaims slots whose owning session IDs are no longer active in `proc[]`.
* **Limitation**: A single active login session that repeatedly invokes `xbegin()` without committing or aborting will exhaust all slots, enforcing intentional backpressure against resource leakage.

---

## 5. Buffering Scope: Held Only for Console and Pipes

* **Supported Output Streams**:
  * **Console writes** (`consolewrite()`): Held in a kernel buffer if the writing process belongs to an active speculation.
  * **Inter-speculation pipe writes** (`pipewrite()`): Held if the reader is outside the speculation.
* **Buffer Capacity Constraints**:
  * `HELDBYTES = 4096`: Maximum bytes of output held per speculation (`kernel/param.h`).
  * `NHELD = 64`: Maximum output records per speculation. Consecutive writes to the same destination are coalesced into a single record.
  * When capacity is reached, further output writes return `-1` to ensure uncommitted data is never prematurely externalized.
* **Non-Buffered / Out-of-Scope Streams**:
  * **Kernel Diagnostic Messages** (`printk()` / `panic()`): Kernel logs bypass the barrier and write directly to UART for debugging.
  * **Intra-Speculation Pipes**: If both reader and writer reside inside the same speculation, bytes bypass externalization holding and can be read immediately (`spec_pull`).
  * **Hardware / Network Peripherals**: Any future device writes outside console and pipe drivers are not intercepted by the barrier.

---

## Summary Matrix

| Resource / Feature | Limit / Boundary | Behavior on Limit Exceeded |
|---|---|---|
| **Memory State** | No rollback | Heap/stack/registers stay modified after `xabort` |
| **Shadow Storage** | 64 blocks (`SPECBLOCKS`) | Operations return `-1`, spec marked `FAILED` |
| **Max Safe File Payload** | 56 KB (58 blocks incl. metadata) | 64 KB requires 66 blocks; fails at 65th block |
| **Concurrent Speculations**| 4 slots (`NSPEC`) | `xbegin()` returns `-1` |
| **Held Output Buffer** | 4096 bytes / 64 records | Write calls return `-1`; data never leaks |
| **Buffering Scope** | Console and cross-spec pipes only | Kernel `printk` and devices bypass barrier |
