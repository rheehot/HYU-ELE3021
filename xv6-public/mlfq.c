#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mlfq.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

extern int sys_uptime(void);

static struct proc* MLFQ_PROC = (struct proc*)-1;

// Check whether given process has runnable threads.
static int
runnable(struct proc* p) {
  struct thread* t;
  for (t = p->threads; t < &p->threads[NTHREAD]; ++t)
    if (t->state == RUNNABLE)
      return t - p->threads;
  return -1;
}

// Initialize stride scheduler.
// First process is MLFQ scheduler.
// Function mlfq_cpu_share moves a process to the stride scheduler,
// and it place in the queue at index after 0.
// It controlls the proportion between stride scheduling process and
// MFLQ scheduling process.
void
stride_init(struct stride* this) {
  int i;
  // Initialize MLFQ scheduler
  this->quantum = 5;
  this->total = 0;
  this->pass[0] = 0;
  this->ticket[0] = MAXTICKET;
  this->queue[0] = MLFQ_PROC;

  // Make queue empty except MLFQ scheduler.
  for (i = 1; i < NPROC; ++i) {
    this->pass[i] = -1;
    this->ticket[i] = 0;
    this->queue[i] = 0;
  }
}

// Append process to the stride scheduler with given proportion of cpu usage.
int
stride_append(struct stride* this, struct proc* p, int usage) {
  int idx;
  float minpass;
  float* pass;
  struct proc** iter;
  // If total proprotion exceeds maximum stride scheduling.
  if (this->total + usage > MAXSTRIDE || usage <= 0)
    return 0;

  // Find empty space.
  for (iter = this->queue; iter != &this->queue[NPROC]; ++iter)
    if (*iter == 0)
      goto found;

  return 0;

found:
  idx = iter - this->queue;
  // Set scheduler information in process.
  p->mlfq.level = -1;
  p->mlfq.index = idx;

  *iter = p;
  this->total += usage;
  this->ticket[0] -= usage;
  this->ticket[idx] = usage;

  // Set pass value of given process
  // with minimum pass value between existing processes.
  minpass = this->pass[0];
  for (pass = this->pass + 1; pass != &this->pass[NPROC]; ++pass) {
    if (*pass != -1 && minpass > *pass) {
      minpass = *pass;
    }
  }

  this->pass[idx] = minpass;
  return 1;
}

// Delete process from stride scheduler.
void
stride_delete(struct stride* this, struct proc* p) {
  int idx = p->mlfq.index;
  int usage = this->ticket[idx];
  this->total -= usage;
  this->ticket[0] += usage;

  this->pass[idx] = -1;
  this->ticket[idx] = 0;
  this->queue[idx] = 0;
}

// Update pass value of given process.
int
stride_update(struct stride* this, struct proc* p) {
  int idx;
  float* pass;
  if (p == MLFQ_PROC)
    idx = 0;
  else
    idx = p->mlfq.index;

  this->pass[idx] += (float)MAXTICKET / this->ticket[idx];

  // If pass value exceeds maximum pass value,
  // substract all pass value with predefined scaling term
  // to maintain them in sufficient range.
  if (this->pass[idx] > MAXPASS)
    for (pass = this->pass; pass != this->pass + NPROC; ++pass)
      if (*pass > 0)
        *pass -= MAXPASS - SCALEPASS;

  return MLFQ_NEXT;
}

// Get next process based on stride scheduling policy.
// Write runnable thread index if exists.
struct proc*
stride_next(struct stride* this, int* tidx) {
  int idx;
  float* iter;
  float* minpass = this->pass;

  // Get process which is runnable and have minimum pass value.
  for (iter = this->pass + 1; iter != &this->pass[NPROC]; ++iter)
    if (*iter != -1 && *minpass > *iter)
      if ((idx = runnable(this->queue[iter - this->pass])) != -1) {
        minpass = iter;
        *tidx = idx;
      }

  return this->queue[minpass - this->pass];
}

// Initialize MLFQ scheduler. 
void
mlfq_init(struct mlfq* this)
{
  int i, j;
  struct proc** iter = &this->queue[0][0];

  static const uint quantum[] = { 5, 10, 20 };
  static const uint expire[] = { 20, 40, 200 };

  for (i = 0; i < NMLFQ; ++i) {
    this->quantum[i] = quantum[i];
    this->expire[i] = expire[i];
    for (j = 0; j < NPROC; ++j, ++iter)
      *iter = 0;

    this->iterstate[i] = this->queue[i];
  }

  // Stride scehduler acts as meta-scheduler,
  // which controls the cpu usage between MLFQ scheduling process
  // and stride scheduling process.
  stride_init(&this->metasched);
}

// Append process to MLFQ scheduler.
int
mlfq_append(struct mlfq* this, struct proc* p, int level)
{
  struct proc** iter;
  // Find empty space.
  for (iter = this->queue[level]; iter != &this->queue[level][NPROC]; ++iter)
    if (*iter == 0)
      goto found;

  return MLFQ_FULL_QUEUE;

found:
  *iter = p;
  // Update scheduler information of given process.
  p->mlfq.level = level;
  p->mlfq.index = iter - this->queue[level];
  p->mlfq.elapsed = 0;
  return MLFQ_SUCCESS;
}

// Pass process to the stride scheduler.
int
mlfq_cpu_share(struct mlfq* this, struct proc* p, int usage)
{
  int level = p->mlfq.level;
  int index = p->mlfq.index;
  if (!stride_append(&this->metasched, p, usage)) {
    return -1;
  }
  // Remove from MLFQ scheduler.
  this->queue[level][index] = 0;
  return 0;
}

// Delete process from MLFQ scheduler.
void
mlfq_delete(struct mlfq* this, struct proc* p)
{
  // If process level is set to -1,
  // it indicates that process is scheduled by stride scheduler.
  if (p->mlfq.level == -1)
    stride_delete(&this->metasched, p);
  else
    this->queue[p->mlfq.level][p->mlfq.index] = 0;  
}

// Update process level by checking elapsed time.
int
mlfq_update(struct mlfq* this, struct proc* p, uint ctime)
{
  int level = p->mlfq.level;
  int index = p->mlfq.index;

  // When process terminated, queue is cleared by method wait().
  if (p->state == ZOMBIE || p->killed)
    return MLFQ_NEXT;

  // If process level is -1, it indicates scheduled by stride scheduler.
  if (level == -1)
    return stride_update(&this->metasched, p);

  // Update pass value of MLFQ scheulder.
  stride_update(&this->metasched, MLFQ_PROC);
  // If avilable time is expired, move the process to the next queue.
  if (level + 1 < NMLFQ && p->mlfq.elapsed >= this->expire[level]) {
    if (mlfq_append(this, p, level + 1) != MLFQ_SUCCESS)
      panic("mlfq: level elevation failed");

    // Remove from current level queue.
    this->queue[level][index] = 0;
    return MLFQ_NEXT;
  }

  // Check process use CPU time of RR time quantum.
  if (ctime - p->mlfq.start < this->quantum[level])
    return MLFQ_KEEP;
  else
    return MLFQ_NEXT;
}

// Get next process with MLFQ scheduling policy.
// If it returns zero, it means nothing runnaable.
// Write runnable thread index to given argument `tidx`.
struct proc*
mlfq_next(struct mlfq* this, int* tidx)
{
  int i, flag, idx;
  struct proc** iter;
  struct proc* p;

  for (i = 0; i < NMLFQ; ++i) {
    // Use flag for enabling all sequence check.
    flag = 1;
    for (iter = this->iterstate[i] + 1;
         flag || iter != this->iterstate[i] + 1;
         ++iter) {
      flag = 0;
      // If exceed the range, begin from first.
      if (iter == &this->queue[i][NPROC])
        iter = this->queue[i];
      // Just runnable process.
      p = *iter;
      if (p == 0 || (idx = runnable(p)) == -1)
        continue;
      
      // Update iterator state and return process.
      this->iterstate[i] = iter;
      *tidx = idx;
      return p;
    }
  }

  // Nothing to runnable.
  return 0;
}

// Boost all process to the top level.
void
mlfq_boost(struct mlfq* this)
{
  int found;
  struct proc* p;
  struct proc** top = this->queue[0];
  struct proc** lower = this->queue[1];

  for (; lower != &this->queue[NMLFQ - 1][NPROC]; ++lower) {
    if (*lower == 0)
      continue;

    found = 0;
    // Find empty space on top level.
    for (; top != &this->queue[0][NPROC]; ++top) {
      if (*top != 0)
        continue;
      
      // Move lower proceee to the top level.
      *top = *lower;
      *lower = 0;
      // Update scheduler information.
      p = *top;
      p->mlfq.level = 0;
      p->mlfq.index = top - this->queue[0];
      p->mlfq.elapsed = 0;

      found = 1;
      break;
    }

    if (!found)
      panic("mlfq boost: could not find empty space of toplevel queue");
  }
}

// MLFQ scheduler.
void
mlfq_scheduler(struct mlfq* this, struct spinlock* lock)
{
  int keep, idx;
  uint start, end, boost, boostunit;
  struct proc* p = 0;
  struct cpu* c = mycpu();
  struct stride* state = &this->metasched;

  idx = 0;
  c->proc = 0;
  boostunit = this->expire[NMLFQ - 1];

  keep = MLFQ_NEXT;
  boost = boostunit;
  for (;;) {
    // Enable interrupts.
    sti();

    acquire(lock);
    do {
      // If previous run commands replace the proc or
      // current process is not runnable.
      if (keep == MLFQ_NEXT || p->threads[p->tidx].state != RUNNABLE) {
        // Get next process from method to run.
        p = stride_next(state, &idx);
        // If given process is MLFQ scheduler,
        // request a new process.
        if (p == MLFQ_PROC)
          p = mlfq_next(this, &idx);

        // If there is nothing runnable.
        if (p == 0) {
          // Update MLFQ pass value for preventing deadlock.
          keep = stride_update(state, MLFQ_PROC);
          break;
        }

        // Update index of the current running thread.
        p->tidx = idx;
      }

      // Switch to chosen process.
      // It is the process's job to relase ptable.lock
      // and then reacquire it before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->threads[p->tidx].state = RUNNING;

      start = sys_uptime();
      p->mlfq.start = start;
      swtch(&(c->scheduler), p->threads[p->tidx].context);
      switchkvm();

      // Update MLFQ states.
      end = sys_uptime();
      p->mlfq.elapsed += end - start;
      keep = mlfq_update(this, p, end);

      // If boosting time arrived.
      if (end > boost) {
        mlfq_boost(this);
        boost += boostunit;
      }

      c->proc = 0;
    } while (0);
    release(lock);
  }
}

// MLFQ state logger
void
mlfq_log(struct mlfq* this, int maxproc)
{
  int i, j;
  struct stride* stride = &this->metasched;
  cprintf("----------\n");
  cprintf("tick: %d\n", sys_uptime());
  for (i = 0; i < maxproc; ++i) {
    cprintf("%p(", stride->queue[i]);
    if (stride->queue[i] != MLFQ_PROC && stride->queue[i]) {
      cprintf("%s, ", stride->queue[i]->name);
    }
    cprintf("%d, %d) ", stride->ticket[i], (int)stride->pass[i]);
  }
  cprintf("\n");
  for (i = 0; i < 3; ++i) {
    for (j = 0; j < maxproc; ++j) {
      cprintf("%p(", this->queue[i][j]);
      if (this->queue[i][j])
        cprintf("%s, %d, %d", this->queue[i][j]->name, this->queue[i][j]->mlfq.start, this->queue[i][j]->mlfq.elapsed);
      cprintf(") ");
    }
    cprintf("\n");
  }
}

// Check whether interrupt yield the process to scheduling CPU or not.
int
mlfq_yieldable(struct mlfq* this, struct proc* p)
{
  int dur = sys_uptime() - p->mlfq.start;
  // yield if it use CPU time of RR time quantum.
  return 
    // for stride scheduler
    (p->mlfq.level == -1 && dur >= this->metasched.quantum)
    // for mlfq scheduler
    || dur >= this->quantum[p->mlfq.level];
}
