#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct
{
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int cpuid()
{
  return mycpu() - cpus;
}

static float get_bjf_rank(struct proc *p)
{
  return p->scheduling_data.bjf.priority_ratio * p->scheduling_data.bjf.priority + p->scheduling_data.bjf.arrival_time_ratio * p->xticks + p->scheduling_data.bjf.executed_cycle_ratio * p->scheduling_data.bjf.executed_cycle + p->scheduling_data.bjf.process_size_ratio * p->sz;
}

void set_proc_sched(struct proc *np)
{
  np->scheduling_data.queue = NO_QUEUE;
  np->scheduling_data.bjf.priority = 3;
  np->scheduling_data.age = np->xticks;
  np->scheduling_data.bjf.executed_cycle = 0;
  np->scheduling_data.bjf.priority_ratio = 1;
  np->scheduling_data.bjf.arrival_time_ratio = 1;
  np->scheduling_data.bjf.executed_cycle_ratio = 1;
  np->scheduling_data.bjf.process_size_ratio = 1;
}

// this function changes the queue of the
// process with given pid
int change_queue(int pid, int new_queue)
{
  struct proc *p;

  if (new_queue == NO_QUEUE)
  {
    if (pid == 1 || pid == 2)
      new_queue = ROUND_ROBIN;
    else if (pid > 1)
      new_queue = LCFS;
    else
      return -1;
  }

  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      p->scheduling_data.queue = new_queue;
      break;
    }
  }
  release(&ptable.lock);
  return 0;
}

void refresh_queue()
{
  struct proc *temp;
  for (temp = ptable.proc; temp < &ptable.proc[NPROC]; temp++)
  {
    if (temp->scheduling_data.queue == NO_QUEUE)
    {
      if (temp->name)
        change_queue(temp->pid, NO_QUEUE);
    }
  }
}

struct proc *get_bjf_proc()
{
  struct proc *res = 0, *temp;
  float min;
  for (temp = ptable.proc; temp < &ptable.proc[NPROC]; temp++)
  {
    if (temp->state != RUNNABLE || temp->scheduling_data.queue != BJF)
      continue;
    float rank = get_bjf_rank(temp);
    if (res == 0 || rank < min)
    {
      res = temp;
      min = rank;
    }
  }
  return res;
}

struct proc *get_rr_proc()
{
  struct proc *res = 0, *temp;
  float min;
  for (temp = ptable.proc; temp < &ptable.proc[NPROC]; temp++)
  {
    if (temp->state != RUNNABLE || temp->scheduling_data.queue != ROUND_ROBIN)
      continue;
    // cprintf("found round robin: %d\n",temp->pid);
    float rank = temp->scheduling_data.age;
    if (res == 0 || rank < min)
    {
      res = temp;
      min = rank;
    }
  }
  return res;
}

struct proc *get_lcfs_proc()
{
  struct proc *res = 0, *temp;
  float max;
  for (temp = ptable.proc; temp < &ptable.proc[NPROC]; temp++)
  {
    if (temp->state != RUNNABLE || temp->scheduling_data.queue != LCFS)
      continue;

    float rank = temp->scheduling_data.age;
    if (res == 0 || rank > max)
    {
      res = temp;
      max = rank;
    }
  }
  return res;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu *
mycpu(void)
{
  int apicid, i;

  if (readeflags() & FL_IF)
    panic("mycpu called with interrupts enabled\n");

  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i)
  {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc *
myproc(void)
{
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

// PAGEBREAK: 32
//  Look in the process table for an UNUSED proc.
//  If found, change state to EMBRYO and initialize
//  state required to run in the kernel.
//  Otherwise return 0.
static struct proc *
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->state == UNUSED)
      goto found;
  }

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  // Allocate kernel stack.
  if ((p->kstack = kalloc()) == 0)
  {
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe *)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint *)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context *)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

// PAGEBREAK: 32
//  Set up first user process.
void userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();

  initproc = p;
  if ((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0; // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if (n > 0)
  {
    if ((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  else if (n < 0)
  {
    if ((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0)
  {
    return -1;
  }

  // Copy process state from proc.
  if ((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0)
  {
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;

    return -1;
  }

  acquire(&tickslock);
  np->xticks = ticks; // creation time for this process
  release(&tickslock);

  set_proc_sched(np); // init scheduler data

  // np->scheduling_data.queue = ROUND_ROBIN;
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for (i = 0; i < NOFILE; i++)
    if (curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  if (strncmp(curproc->name, "foo2", 5) == 0)
  {
    np->scheduling_data.queue = ROUND_ROBIN;
  }
  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if (curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for (fd = 0; fd < NOFILE; fd++)
  {
    if (curproc->ofile[fd])
    {
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
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->parent == curproc)
    {
      p->parent = initproc;
      if (p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if (p->parent != curproc)
        continue;
      havekids = 1;
      if (p->state == ZOMBIE)
      {
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || curproc->killed)
    {
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock); // DOC: wait-sleep
  }
}

void run_proc(struct proc *p, struct cpu *c)
{
  acquire(&ptable.lock);
  // Switch to chosen process.  It is the process's job
  // to release ptable.lock and then reacquire it
  // before jumping back to us.
  c->proc = p;
  switchuvm(p);
  p->state = RUNNING;

  swtch(&(c->scheduler), p->context);

  p->scheduling_data.bjf.executed_cycle += 0.1;

  switchkvm();

  // Process is done running for now.
  // It should have changed its p->state before coming back.
  c->proc = 0;
  release(&ptable.lock);
}

// PAGEBREAK: 42
//  Per-CPU process scheduler.
//  Each CPU calls scheduler() after setting itself up.
//  Scheduler never returns.  It loops, doing:
//   - choose a process to run
//   - swtch to start running that process
//   - eventually that process transfers control
//       via swtch back to the scheduler.
void scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  // cprintf("Here-1\n");
  c->proc = 0;

  for (;;)
  {
    // cprintf("Here-2\n");

    refresh_queue();
    // Enable interrupts on this processor.
    sti();

    while (1)
    {
      acquire(&ptable.lock);
      p = get_rr_proc();
      if (p == 0)
        p = get_lcfs_proc();
      if (p == 0)
        p = get_bjf_proc();
      release(&ptable.lock);
      if (p != 0)
        break;
    }
    run_proc(p, c);

    acquire(&ptable.lock);
    acquire(&tickslock);
    p->scheduling_data.age = ticks;
    release(&tickslock);
    release(&ptable.lock);
  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void sched(void)
{
  int intena;
  struct proc *p = myproc();

  if (!holding(&ptable.lock))
    panic("sched ptable.lock");
  if (mycpu()->ncli != 1)
  {
    cprintf("ncli : %d\n", mycpu()->ncli);
    panic("sched locks");
  }
  if (p->state == RUNNING)
    panic("sched running");
  if (readeflags() & FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void yield(void)
{
  acquire(&ptable.lock); // DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first)
  {
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
void sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  if (p == 0)
    panic("sleep");

  if (lk == 0)
    panic("sleep without lk");

  if (lk != &ptable.lock)
  {
    acquire(&ptable.lock);
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  p->chan = 0;

  // Reacquire original lock.
  if (lk != &ptable.lock)
  { // DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

// PAGEBREAK!
//  Wake up all processes sleeping on chan.
//  The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if (p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      p->killed = 1;
      // Wake process from sleep if necessary.
      if (p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

// PAGEBREAK: 36
//  Print a process listing to console.  For debugging.
//  Runs when user types ^P on console.
//  No lock to avoid wedging a stuck machine further.
void procdump(void)
{
  int i;
  struct proc *p;
  uint pc[10];

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->state == UNUSED)
      continue;
    if (p->state == SLEEPING)
    {
      getcallerpcs((uint *)p->context->ebp + 2, pc);
      for (i = 0; i < 10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

// this function iteretes trhough all
// proc and if their parent is same as
// current process grand parent then it
// is uncle of proc
int sys_get_uncle_count(void)
{
  acquire(&ptable.lock);
  int count = 0;
  struct proc *my_proc = myproc(); // Get the current process
  struct proc *curr_proc;

  for (curr_proc = ptable.proc; curr_proc < &ptable.proc[NPROC]; curr_proc++)
  {
    if (curr_proc->state == UNUSED || curr_proc->state == EMBRYO || curr_proc->pid == my_proc->pid || curr_proc->pid == my_proc->parent->pid) // escaping incomplete proc
      continue;

    if (curr_proc->parent && curr_proc->parent->pid == my_proc->parent->parent->pid)
    {
      cprintf("Found uncle with pid:%d and name: %s\n", curr_proc->pid, curr_proc->name);
      count++;
    }
  }
  release(&ptable.lock);

  return count;
}

// This function checks for aging
void do_aging(int tiks)
{
  struct proc *p;
  acquire(&ptable.lock);

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if (p->state == RUNNABLE && p->scheduling_data.queue != ROUND_ROBIN)
    {
      // cprintf("aging %d:%d \n", tiks, p->scheduling_data.age);
      if (tiks - p->scheduling_data.age > AGED_OUT)
      {
        // cprintf("%d\t%d\t%d\t%d\n", tiks, p->scheduling_data.age, p->pid, p->scheduling_data.queue);
        release(&ptable.lock);
        change_queue(p->pid, p->scheduling_data.queue == LCFS ? ROUND_ROBIN : LCFS);
        acquire(&ptable.lock);
        p->scheduling_data.age = ticks;
      }
    }

  release(&ptable.lock);
}

int sys_set_bjf_for_process(void)
{
  int pid;
  float priority_ratio, arrival_time_ratio, executed_cycle_ratio, process_size_ratio;
  if (argint(0, &pid) < 0 ||
      argfloat(1, &priority_ratio) < 0 ||
      argfloat(2, &arrival_time_ratio) < 0 ||
      argfloat(3, &executed_cycle_ratio) < 0 ||
      argfloat(4, &process_size_ratio) < 0)
  {
    return -1;
  }

  acquire(&ptable.lock);
  struct proc *p;
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      p->scheduling_data.bjf.priority_ratio = priority_ratio;
      p->scheduling_data.bjf.arrival_time_ratio = arrival_time_ratio;
      p->scheduling_data.bjf.executed_cycle_ratio = executed_cycle_ratio;
      p->scheduling_data.bjf.process_size_ratio = process_size_ratio;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return 0;
}

int sys_set_bjf_for_all(void)
{
  float priority_ratio, arrival_time_ratio, executed_cycle_ratio, process_size_ratio;
  if (argfloat(0, &priority_ratio) < 0 ||
      argfloat(1, &arrival_time_ratio) < 0 ||
      argfloat(2, &executed_cycle_ratio) < 0 ||
      argfloat(3, &process_size_ratio) < 0)
  {
    return -1;
  }

  acquire(&ptable.lock);
  struct proc *p;
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    p->scheduling_data.bjf.priority_ratio = priority_ratio;
    p->scheduling_data.bjf.arrival_time_ratio = arrival_time_ratio;
    p->scheduling_data.bjf.executed_cycle_ratio = executed_cycle_ratio;
    p->scheduling_data.bjf.process_size_ratio = process_size_ratio;
  }
  release(&ptable.lock);
  return 0;
}

int sys_change_queue(void)
{
  int pid, queue;
  if (argint(0, &pid) < 0 || argint(1, &queue) < 0)
  {
    return -1;
  }
  struct proc *p;
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if (p->pid == pid)
      p->scheduling_data.age = ticks;
  // cprintf("sys change queue!\n");
  change_queue(pid, queue);
  return 0;
}

int sys_ps(void)
{
  cprintf("proc name\tPID\tState\t\tQueue\tCycle\tArrival\tPriority\tR_Party\tR_arvl\tR_Exec\tR_Size\tRank\n");
  cprintf("--------------------------------------------------------------------------------------------------------------------------------\n");
  acquire(&ptable.lock);
  struct proc *p;
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->state == RUNNABLE || p->state == RUNNING || p->state == SLEEPING)
    {
      cprintf(p->name);
      cprintf("\t\t");
      cprintf("%d", p->pid);
      cprintf("\t");
      cprintf(p->state == RUNNABLE ? "RUNNABLE" : p->state == RUNNING ? "RUNNING\t"
                                              : p->state == SLEEPING  ? "SLEEPING"
                                                                      : "ZOMBIE");
      cprintf("\t");
      cprintf("%d\t", p->scheduling_data.queue);
      cprintf("%d\t%d\t%d\t\t%d\t%d\t%d\t%d\t%d\n", (int)p->scheduling_data.bjf.executed_cycle, (int)p->xticks,
              (int)p->scheduling_data.bjf.priority, (int)p->scheduling_data.bjf.priority_ratio, (int)p->scheduling_data.bjf.arrival_time_ratio,
              (int)p->scheduling_data.bjf.executed_cycle_ratio, (int)p->scheduling_data.bjf.process_size_ratio, (int)get_bjf_rank(p));
    }
  }
  release(&ptable.lock);
  return 0;
}
