#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

#define PIPESIZE 512

struct pipe {
  struct spinlock lock;
  char data[PIPESIZE];
  uint nread;    // number of bytes read
  uint nwrite;   // number of bytes written
  int readopen;  // read fd is still open
  int writeopen; // write fd is still open
  int spec;      // speculation the pipe was created in, 0 if none
  int held;      // held writes waiting for xcommit or xabort
};

int
pipealloc(struct file **f0, struct file **f1)
{
  struct pipe *pi;

  pi = 0;
  *f0 = *f1 = 0;
  if ((*f0 = filealloc()) == 0 || (*f1 = filealloc()) == 0)
    goto bad;
  if ((pi = (struct pipe *)kalloc()) == 0)
    goto bad;
  pi->readopen = 1;
  pi->writeopen = 1;
  pi->nwrite = 0;
  pi->nread = 0;
  pi->spec = myproc()->spec;
  pi->held = 0;
  initlock(&pi->lock, "pipe");
  (*f0)->type = FD_PIPE;
  (*f0)->readable = 1;
  (*f0)->writable = 0;
  (*f0)->pipe = pi;
  (*f1)->type = FD_PIPE;
  (*f1)->readable = 0;
  (*f1)->writable = 1;
  (*f1)->pipe = pi;
  return 0;

bad:
  if (pi)
    kfree((char *)pi);
  if (*f0)
    fileclose(*f0);
  if (*f1)
    fileclose(*f1);
  return -1;
}

void
pipeclose(struct pipe *pi, int writable)
{
  acquire(&pi->lock);
  if (writable) {
    pi->writeopen = 0;
    wakeup(&pi->nread);
  } else {
    pi->readopen = 0;
    wakeup(&pi->nwrite);
  }
  if (pi->readopen == 0 && pi->writeopen == 0 && pi->held == 0) {
    release(&pi->lock);
    kfree((char *)pi);
  } else
    release(&pi->lock);
}

// A speculation is holding a write for this pipe.  Held data counts as
// a writer that is still open, so the reader waits for it instead of
// seeing end-of-file, and the pipe is not freed before it is delivered.
void
pipe_hold(struct pipe *pi)
{
  acquire(&pi->lock);
  pi->held++;
  release(&pi->lock);
}

// The held write was delivered (xcommit) or dropped (xabort).
void
pipe_unhold(struct pipe *pi)
{
  acquire(&pi->lock);
  pi->held--;
  wakeup(&pi->nread);
  if (pi->readopen == 0 && pi->writeopen == 0 && pi->held == 0) {
    release(&pi->lock);
    kfree((char *)pi);
  } else
    release(&pi->lock);
}

// Write n bytes from addr (a user address if user_src is set).
int
pipewrite(struct pipe *pi, int user_src, uint64 addr, int n)
{
  int i = 0;
  struct proc *pr = myproc();

  // A speculative writer whose reader is outside its speculation: hold
  // the bytes until commit.  A pipe created inside the same speculation
  // is not held, or every pipeline inside it would hang.
  if (pr->spec != 0 && pr->spec != pi->spec)
    return spec_hold(pi, user_src, addr, n);

  acquire(&pi->lock);
  while (i < n) {
    if (pi->readopen == 0 || killed(pr)) {
      release(&pi->lock);
      return -1;
    }
    if (pi->nwrite == pi->nread + PIPESIZE) { //DOC: pipewrite-full
      wakeup(&pi->nread);
      sleep_prepare(&pi->nwrite);
      release(&pi->lock);
      sleep();
      acquire(&pi->lock);
    } else {
      char ch;
      if (either_copyin(&ch, user_src, addr + i, 1) == -1) {
        if (i == 0)
          i = -1;
        break;
      }
      pi->data[pi->nwrite++ % PIPESIZE] = ch;
      i++;
    }
  }
  wakeup(&pi->nread);
  release(&pi->lock);

  return i;
}

int
piperead(struct pipe *pi, uint64 addr, int n)
{
  int i;
  struct proc *pr = myproc();
  char ch;

  acquire(&pi->lock);
  while (pi->nread == pi->nwrite &&
         (pi->writeopen || pi->held)) { //DOC: pipe-empty
    if (killed(pr)) {
      release(&pi->lock);
      return -1;
    }
    sleep_prepare(&pi->nread); //DOC: piperead-sleep
    release(&pi->lock);
    sleep();
    acquire(&pi->lock);
  }
  for (i = 0; i < n; i++) { //DOC: piperead-copy
    if (pi->nread == pi->nwrite)
      break;
    ch = pi->data[pi->nread % PIPESIZE];
    if (copyout(pr->pagetable, pr->sz, addr + i, &ch, 1) == -1) {
      if (i == 0)
        i = -1;
      break;
    }
    pi->nread++;
  }
  wakeup(&pi->nwrite); //DOC: piperead-wakeup
  release(&pi->lock);
  return i;
}
