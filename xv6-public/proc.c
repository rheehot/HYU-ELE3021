#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mlfq.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

struct mlfq mlfq;

static struct proc *initproc;

int nextpid = 1;
int nexttid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
  mlfq_init(&mlfq);
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  struct thread* t;
  char *sp;
  int off;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  // Set default process, thread states.
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->tidx = 0;

  t = p->threads;
  t->state = EMBRYO;
  t->tid = nexttid++;

  // Add process to MLFQ scheulder.
  mlfq_append(&mlfq, p, 0);
  release(&ptable.lock);

  // Reset stacks.
  for (off = 0; off < NTHREAD; ++off) {
    p->kstacks[off] = 0;
    p->ustacks[off] = 0;
  }

  // Allocate kernel stack.
  if((p->kstacks[0] = kalloc()) == 0){
    p->state = UNUSED;
    t->state = UNUSED;
    return 0;
  }
  t->kstack = p->kstacks[0];
  sp = t->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *t->tf;
  t->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *t->context;
  t->context = (struct context*)sp;
  memset(t->context, 0, sizeof *t->context);
  t->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  struct thread *t;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;

  t = p->threads;
  memset(t->tf, 0, sizeof(*t->tf));
  t->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  t->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  t->tf->es = t->tf->ds;
  t->tf->ss = t->tf->ds;
  t->tf->eflags = FL_IF;
  t->tf->esp = PGSIZE;
  t->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;
  t->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid, tmp;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->threads->kstack);
    np->threads->kstack = 0;
    np->threads->state = UNUSED;
    np->kstacks[0] = 0;
    np->state = UNUSED;
    return -1;
  }

  np->sz = curproc->sz;
  np->parent = curproc;

  np->tidx = 0;
  // Copy user stack pool.
  for (i = 0; i < NTHREAD; ++i)
    np->ustacks[i] = curproc->ustacks[i];

  // Swap stacks of current thread index and index 0.
  tmp = np->ustacks[0];
  np->ustacks[0] = np->ustacks[curproc->tidx];
  np->ustacks[curproc->tidx] = tmp;

  // Copy trapframe, it will return to instruction `retn` of fork syscall.
  *np->threads->tf = *curproc->threads[curproc->tidx].tf;

  // Clear %eax so that fork returns 0 in the child.
  np->threads->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;
  np->threads->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  struct thread *t;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  for (t = curproc->threads; t < &curproc->threads[NTHREAD]; t++)
    if (t->state != UNUSED)
      t->state = ZOMBIE;

  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  struct thread *t;
  int havekids, pid, off;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        // Free all zombie threads.
        for (t = p->threads; t < &p->threads[NTHREAD]; t++) {
          off = t - p->threads;
          if (p->kstacks[off] != 0) {
            kfree(p->kstacks[off]);
            p->kstacks[off] = 0;
            p->ustacks[off] = 0;
          }
          t->kstack = 0;
          t->state = UNUSED;
          t->tid = 0;
        }
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        // Delete process from MLFQ.
        mlfq_delete(&mlfq, p);
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  mlfq_scheduler(&mlfq, &ptable.lock);
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();
  struct thread *t;

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");

  t = &p->threads[p->tidx];
  if(t->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&t->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Context switching between threads.
// It do not reload the cr3 but update only address of kernel stack
// for previlege escalation.
void
next_thread(struct proc* p) {
  int intena;
  struct thread* iter;
  struct thread* t = &p->threads[p->tidx];
  acquire(&ptable.lock);

  // Find runnable thread.
  for (iter = t + 1; ; ++iter) {
    if (iter == &p->threads[NTHREAD])
      iter = p->threads;
    if (iter == t) {
      // If runnable thread does not exist and
      // current thread is also not runnable.
      if (t->state != RUNNING) {
        sched();
        panic("next_thread cannot run thread");
      }
      break;
    }

    // If runnable thread found.
    if (iter->state == RUNNABLE) {
      t->state = RUNNABLE;
      iter->state = RUNNING;

      // Update running thread index.
      p->tidx = iter - p->threads;
      switch_trap_kstack(p);

      // Context switch.
      intena = mycpu()->intena;
      swtch(&t->context, iter->context);
      mycpu()->intena = intena;
      break;
    }
  }
  release(&ptable.lock);
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p;
  acquire(&ptable.lock);  //DOC: yieldlock
  p = myproc();
  p->threads[p->tidx].state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  struct thread *t;

  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  t = &p->threads[p->tidx];
  t->chan = chan;
  t->state = SLEEPING;

  sched();

  // Tidy up.
  t->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;
  struct thread *t;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == RUNNABLE)
      for (t = p->threads; t < &p->threads[NTHREAD]; ++t)
        if (t->state == SLEEPING && t->chan == chan)
          t->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;
  struct thread *t;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      for (t = p->threads; t < &p->threads[NTHREAD]; t++)
        // Wake process from sleep if necessary.
        if (t->state == SLEEPING)
          t->state = RUNNABLE;

      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  struct thread *t;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    
    t = &p->threads[p->tidx];
    if(t->state >= 0 && t->state < NELEM(states) && states[t->state])
      state = states[t->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(t->state == SLEEPING){
      getcallerpcs((uint*)t->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

// Return scheduler level of process.
int
getlev(void)
{
  struct proc* p = myproc();
  if (p == 0)
    return -1;
  
  return p->mlfq.level;
}

// Move process from MLFQ scheulder to stride scheduler
// with given proportion of CPU usage.
int
set_cpu_share(int percent)
{
  return mlfq_cpu_share(&mlfq, myproc(), percent);
}

// End of thread, make thread state zombie
// and update user thread.
void
thread_epilogue(void) {
  struct proc *p;
  struct thread *t;

  acquire(&ptable.lock);

  p = myproc();
  t = &p->threads[p->tidx];

  // Update thread state.
  t->state = ZOMBIE;
  wakeup1((void*)t->tid);

  sched();
  panic("thread_epilogue: unreachable statements");
}

// Create thread with given user thread structure
// and start routine.
int
thread_create(int *tid, void*(*start_routine)(void*), void *arg) {
  int tidx, sz;
  char *sp;
  struct proc *p;
  struct thread *t;

  acquire(&ptable.lock);

  // Find unused thread slot.
  p = myproc();
  for (t = p->threads; t < &p->threads[NTHREAD]; ++t)
    if (t->state == UNUSED)
      goto find;
  
  release(&ptable.lock);
  return -1;

find:
  tidx = t - p->threads;
  t->tid = nexttid++;

  // Allocate new kernel stack for isolating space.
  if (p->kstacks[tidx] == 0 && (p->kstacks[tidx] = kalloc()) == 0) {
    t->tid = 0;
    t->state = UNUSED;
    release(&ptable.lock);
    return -1;
  }
  // Assign allocated stack.
  t->kstack = p->kstacks[tidx];
  sp = t->kstack + KSTACKSIZE;

  // Copy trapframe for recovering trivial bytes
  // (segment registers, etc..)
  sp -= sizeof *t->tf;
  t->tf = (struct trapframe*)sp;
  *t->tf = *p->threads[p->tidx].tf;

  // Second return address is trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  // Clear context.
  sp -= sizeof *t->context;
  t->context = (struct context*)sp;
  memset(t->context, 0, sizeof *t->context);

  // First return address is forkret.
  t->context->eip = (uint)forkret;

  // Allocate user stack.
  if (p->ustacks[tidx] != 0)
    sz = p->ustacks[tidx];
  else {
    sz = PGROUNDUP(p->sz);
    if ((sz = allocuvm(p->pgdir, sz, sz + PGSIZE)) == 0) {
      t->kstack = 0;
      t->tid = 0;
      t->state = UNUSED;
      release(&ptable.lock);
      return -1;
    }
    
    p->sz = sz;
    p->ustacks[tidx] = sz;
  }

  // Write argument for start routine.
  sp = (char*)sz;
  sp -= 4;
  *(uint*)sp = (uint)arg;

  // Return address of start routine.
  // This is just immitation, because user cannot access
  // kernel code of thread_epilogue,
  // so the call of syscall thread_exit is necessary on user level code.
  sp -= 4;
  *(uint*)sp = (uint)thread_epilogue;

  // Third return address is start_routine.
  t->tf->esp = (uint)sp;
  t->tf->eip = (uint)start_routine;

  // Initialize user thread structure.
  *tid = t->tid;

  t->retval = 0;
  t->state = RUNNABLE;
  release(&ptable.lock);
  return 0;
}

// Exit thread, write return value and run epilogue of thread.
void
thread_exit(void *retval) {
  struct proc *p = myproc();
  p->threads[p->tidx].retval = retval;
  thread_epilogue();
}

// Wait until thread is done.
// It acts like `wait` on exit process.
// It clean up the exit thread and write the return value.
int
thread_join(int tid, void **retval) {
  struct proc* p;
  struct thread* t;

  acquire(&ptable.lock);
  // Searching given thread ID.
  for (p = ptable.proc; p < &ptable.proc[NPROC]; ++p)
    if (p->state == RUNNABLE)
      for (t = p->threads; t < &p->threads[NTHREAD]; ++t)
        if (t->tid == tid)
          goto found;    

  release(&ptable.lock);
  return -1;

found:
  // Wait until target thread is done.
  do {
    if (t->state == ZOMBIE)
      break;

    sleep((void*)tid, &ptable.lock);
  } while (0);

  // Write return value.
  *retval = t->retval;

  // Free exit thread.
  t->kstack = 0;
  t->state = UNUSED;
  t->tid = 0;
  t->retval = 0;

  release(&ptable.lock);
  return 0;
}
