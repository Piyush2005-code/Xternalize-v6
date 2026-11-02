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

## 2. Inode Allocation, Synchronization & Permission Checks

### Mode Structure (Strictly 6 bits: Owner + Other)
No group concept is used. Permissions are 6 bits total:
```
[ Owner: R | W | X ]  [ Other: R | W | X ]
      (bits 5..3)           (bits 2..0)
```
* `075` (binary `111 101`): Owner `rwx` (7), Other `r-x` (5) — used for executables and directories.
* `064` (binary `110 100`): Owner `rw-` (6), Other `r--` (4) — used for regular data files.

### `kernel/fs.c`
* **`ialloc(uint dev, short type)`**:
  Initializes new on-disk inodes with `dip->uid = myproc()->uid` and default mode (`075` for directories, `064` for files).
* **`iupdate(struct inode *ip)`**:
  Persists in-memory `ip->uid` and `ip->mode` to disk buffer `dip->uid` and `dip->mode`.
* **`ilock(struct inode *ip)`**:
  Loads on-disk `dip->uid` and `dip->mode` into in-memory `ip->uid` and `ip->mode`.
* **`check_perm(struct inode *ip, int perm)`**:
  Permission resolution function:
  - **Permissions do NOT apply to admin:** If `p->uid == 0`, immediately grants access (`return 1`).
  - If process is file owner (`p->uid == ip->uid`), evaluates against Owner bits (`(ip->mode >> 3) & 07`).
  - Otherwise, evaluates against Other bits (`ip->mode & 07`).
  - Bit masks: `PERM_X (0x1)`, `PERM_W (0x2)`, `PERM_R (0x4)`.

---

## 3. Filesystem Builder Initialization

### `mkfs/mkfs.c`
* Updated line 88 assertion: relaxed `assert((BSIZE % sizeof(struct dinode)) == 0)` to `assert(sizeof(struct dinode) <= BSIZE)` to accommodate the updated `dinode` size.
* **`ialloc()`**:
  Initializes build-time inodes with `uid = 0` (admin).
* **Binary file packaging**:
  Executable binaries (`UPROGS`) are assigned permission mode `075` (`rwxr-x`), while data files (like `README`) receive `064` (`rw-r--`).

---

## 4. Syscall Privilege Table (Built-in Command Privileges)

### `kernel/syscall.c`
* Added `syscall_privileges[]` table mapping each system call to its required privilege:
  - `PRIV_ADMIN`: `SYS_mknod`, `SYS_setuid`, `SYS_sync`
  - `PRIV_USER`: `SYS_fork`, `SYS_exit`, `SYS_wait`, `SYS_pipe`, `SYS_read`, `SYS_kill`, `SYS_exec`, `SYS_fstat`, `SYS_chdir`, `SYS_dup`, `SYS_getpid`, `SYS_sbrk`, `SYS_pause`, `SYS_uptime`, `SYS_open`, `SYS_write`, `SYS_unlink`, `SYS_link`, `SYS_mkdir`, `SYS_close`, `SYS_getuid`
* **In `syscall()`**:
  Checks `p->uid`. If an unprivileged user (`p->uid != 0`) invokes a `PRIV_ADMIN` syscall:
  - Prints: `printk("%d %s: syscall %d denied for uid %d\n", p->pid, p->name, num, p->uid);`
  - Returns `-1` directly in `p->trapframe->a0`.
* **Persistence:** Because this table is compiled directly into kernel source code, command privileges persist automatically across restarts.

---

## 5. Permission Enforcement in Syscalls

### `kernel/sysfile.c`
* **`sys_open()`**:
  - Requires `PERM_R` if opened for reading (`!(omode & O_WRONLY)`).
  - Requires `PERM_W` if opened for writing (`(omode & O_WRONLY) || (omode & O_RDWR) || (omode & O_TRUNC)`).
* **`create()`**:
  - Enforces write permission on the parent directory `check_perm(dp, PERM_W)`.
  - For existing files, checks write permission on the target inode.
  - Sets `ip->uid = myproc()->uid` and mode `075` (dir) or `064` (file).
* **`sys_unlink()`**:
  - Enforces `PERM_W` on parent directory `dp`.
  - Enforces `PERM_W` on target inode `ip`.
* **`sys_write()`**:
  - Enforces `PERM_W` when writing to an inode.

### `kernel/exec.c`
* **`kexec()`**:
  - Enforces `PERM_X` on executable binary inode before loading ELF segments.
