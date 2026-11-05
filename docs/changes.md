# File Permissions and Privilege Levels Implementation

This document details the file permission and system call privilege system implemented for user/admin logins.

---

## 1. On-Disk and In-Memory Inode Layout

### `kernel/fs.h` (`struct dinode`)
Added persistent fields `uid` and `mode` to the on-disk inode structure:
```c
struct dinode {
  short type;              // File type
  short major;             // Major device number (T_DEVICE only)
  short minor;             // Minor device number (T_DEVICE only)
  short nlink;             // Number of links to inode in file system
  uint size;               // Size of file (bytes)
  uint addrs[NDIRECT + 1]; // Data block addresses
  short uid;               // Owner user ID (0: admin, 1: user)
  short mode;              // Permission mode bits (6 bits: Owner + Other)
};
```
* **Disk Layout Impact:** Changing `struct dinode` from 64 to 68 bytes updates `IPB` (`BSIZE / sizeof(struct dinode) = 15`). A `make clean && make fs.img` generates the filesystem image conforming to this new layout.
* `mkfs.c` enforces `static_assert(sizeof(struct dinode) == 68)` at build time.

### `kernel/file.h` (`struct inode`)
Mirrored `uid` and `mode` in the in-memory inode structure:
```c
struct inode {
  ...
  short uid;  // Owner user ID (0: admin, 1: user)
  short mode; // Permission mode bits (6 bits: Owner + Other)
};
```

---

## 2. Permission Modes & Inode Allocation

### Mode Bit Structure (6 bits: Owner + Other)
No group concept is used. Permissions are 6 bits total:
```
[ Owner: R | W | X ]  [ Other: R | W | X ]
      (bits 5..3)           (bits 2..0)
```
Constants are defined in `kernel/fs.h`:
* `MODE_DIR  (075)`: Owner `rwx` (7), Other `r-x` (5) — subdirectories.
* `MODE_EXEC (075)`: Owner `rwx` (7), Other `r-x` (5) — executable system binaries.
* `MODE_FILE (064)`: Owner `rw-` (6), Other `r--` (4) — regular data files (e.g. `README`).
* `MODE_DEV  (066)`: Owner `rw-` (6), Other `rw-` (6) — device nodes (e.g. `/console`), allowing all users console I/O.
* `MODE_ROOT (077)`: Owner `rwx` (7), Other `rwx` (7) — root directory `/`, allowing users to create files in xv6's flat tree while individual files remain protected by their own modes.

### Defaults Set in `ialloc` (`kernel/fs.c`)
Default modes and ownership are assigned in one single place: `ialloc()`:
* `dip->uid = myproc() ? myproc()->uid : 0`
* `dip->mode = (type == T_DIR) ? MODE_DIR : ((type == T_DEVICE) ? MODE_DEV : MODE_FILE)`
`create()` relies on `ialloc` setting the on-disk inode and `ilock` loading it into memory.

### Permission Checking (`check_perm` in `kernel/fs.c`)
* **Admin bypass:** Permissions do NOT apply to admin (`uid == 0`). Admin always has full access.
* **Owner:** If `p->uid == ip->uid`, evaluates `(ip->mode >> 3) & 07`.
* **Other:** Evaluates `ip->mode & 07`.
* **Bit masks:** `PERM_X (0x1)`, `PERM_W (0x2)`, `PERM_R (0x4)`.

---

## 3. Syscall Privilege Enforcement (Fails Closed)

### `kernel/syscall.c`
* Added `syscall_privileges[]` table mapping each system call to its required privilege:
  - `PRIV_ADMIN (0)`: Default for all unlisted entries (fails closed). Explicitly: `SYS_mknod`, `SYS_setuid`, `SYS_sync`.
  - `PRIV_USER  (1)`: General syscalls allowed for all users (`SYS_fork`, `SYS_exit`, `SYS_wait`, `SYS_pipe`, `SYS_read`, `SYS_kill`, `SYS_exec`, `SYS_fstat`, `SYS_chdir`, `SYS_dup`, `SYS_getpid`, `SYS_sbrk`, `SYS_pause`, `SYS_uptime`, `SYS_open`, `SYS_write`, `SYS_unlink`, `SYS_link`, `SYS_mkdir`, `SYS_close`, `SYS_getuid`).
* `_Static_assert(NELEM(syscall_privileges) == NELEM(syscalls))` guarantees that any newly added syscall must be categorized.
* **Denial Behavior:** Silently returns `-1` without printing to the console, preventing information leakage and console spam.

### Process Management Protection (`kernel/proc.c`, `kernel/sysproc.c`)
* **`sys_kill` / `kkill`:**
  - Non-admin processes can only kill processes with matching UID (`target->uid == cur->uid`).
  - Non-admin processes are strictly forbidden from killing `pid 1` (`init`).
* **`sys_setuid`:**
  - Validates `uid == 0 || uid == 1`, avoiding truncation bugs with 16-bit storage.

---

## 4. Filesystem Permission Enforcement

### `kernel/sysfile.c`
* **`sys_open()`**:
  - Enforces `PERM_R` on read mode (`!(omode & O_WRONLY)`).
  - Enforces `PERM_W` only when write access is requested (`(omode & O_WRONLY) || (omode & O_RDWR) || (omode & O_TRUNC)`).
  - Calling `open(f, O_CREATE | O_RDONLY)` on an existing readable file succeeds without requiring write permission.
* **`create()`**:
  - Enforces write permission on the parent directory: `check_perm(dp, PERM_W)`.
  - If target file already exists, returns it directly to let `sys_open` check permissions against `omode`.
* **`sys_unlink()`**:
  - Enforces `PERM_W` on parent directory `dp`.
  - Enforces `PERM_W` on target file `ip`.
* **`sys_link()`**:
  - Enforces `PERM_W` on target directory `dp` after `ilock(dp)`.
* **`sys_write()`**:
  - Relies on `sys_open()` write enforcement; does not re-check permissions in the hot write path.

### `kernel/exec.c`
* **`kexec()`**:
  - Enforces `PERM_X` on executable binary inodes before loading ELF segments.

---

## 5. Design Decisions & Out-of-Scope Items

* **Root Directory Mode (`077`)**: Since xv6 has no `/home` or `/tmp`, root is world-writable so non-admin users can create files. Existing admin binaries and files are still protected by their own non-writable modes.
* **Device Modes (`066`)**: `/console` is world-readable/writable so user shells have functioning stdin/stdout/stderr.
* **Directory Search (`x`) Bit**: In standard Unix, directory traversal checks the `x` bit. In this project scope, directory traversal in `namei` / `dirlookup` is not gated by `x` to avoid breaking path resolution.
* **`chmod` and `stat`**: Neither a `chmod` syscall nor `uid`/`mode` exposure in `struct stat` is implemented (out of scope). Newly created user files default to `MODE_FILE (064)`, meaning users cannot make newly created files executable.
