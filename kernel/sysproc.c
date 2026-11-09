#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0; // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  if (t == SBRK_EAGER || n < 0) {
    if (growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if (addr + n < addr)
      return -1;
    if (addr + n > TRAPFRAME)
      return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if (n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n) {
    if (killed(myproc())) {
      release(&tickslock);
      return -1;
    }
    sleep_prepare(&ticks);
    release(&tickslock);
    sleep();
    acquire(&tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_getuid(void)
{
  return myproc()->uid;
}

uint64
sys_setuid(void)
{
  int uid;
  argint(0, &uid);
  if (uid < 0 || uid > 1)
    return -1;
  struct proc *p = myproc();
  // Only admin (uid == 0) can change uid
  if (p->uid != 0 && uid != p->uid)
    return -1;
  p->uid = uid;
  return 0;
}

// Externalization barrier: see kernel/spec.c.

uint64
sys_xbegin(void)
{
  return spec_begin();
}

uint64
sys_xjoin(void)
{
  int id;

  argint(0, &id);
  return spec_join(id);
}

uint64
sys_xcommit(void)
{
  return spec_end(1);
}

uint64
sys_xabort(void)
{
  return spec_end(0);
}

// setsid(): make the caller a login-session leader, its session id its
// own pid.  The shell calls this once at startup; children inherit the
// id across fork, so a whole login session shares it.
uint64
sys_setsid(void)
{
  struct proc *p = myproc();

  acquire(&p->lock);
  p->sid = p->pid;
  release(&p->lock);
  return 0;
}

// crashat(n): make the n-th crash point of the next xcommit panic (see
// kernel/speccommit.c); 0 turns it off.  Admin only: it takes the kernel down.
uint64
sys_crashat(void)
{
  int n;

  argint(0, &n);
  crash_set(n);
  return 0;
}
