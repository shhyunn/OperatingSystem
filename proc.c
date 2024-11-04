#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"


struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);
//static char* print_unsigned(unsigned int x);
static char* str_indent(char* index);
static char* int_indent(int num);
static char* struct_indent(struct uint2 vruntime);
static char* int_to_str(int num);

int weights[40] = {
	88761, 71755, 56483, 46273, 36291,
	29154, 23254, 18705, 14949, 11916,
	9548, 7620, 6100, 4904, 3906,
	3121, 2501, 1991, 1586, 1277,
	1024, 820, 655, 526, 423,
	355, 272, 215, 172, 137,
	110, 87, 70, 56, 45,
	36, 29, 23, 18, 15
};

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
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
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  release(&ptable.lock);
 
  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
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
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");
  p->priority = 20;

  p->weight = weights[p->priority];
  p->vruntime.high = 0;
  p->vruntime.low = 0;
  //p->vruntime = 0;
  p->runtime = 0;
  p->timeslice = 0;

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;
  // p->priority = 20;

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
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;
  np->priority = curproc->priority;
  np->runtime = 0;
  np->vruntime = curproc->vruntime;
  np->timeslice = 0;
  np->weight= weights[np->priority];

  acquire(&ptable.lock);
 
 np->state = RUNNABLE;

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
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
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
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.

/*
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();
    

    int sum_weight = 0;

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    p = ptable.proc;

    uint min_vruntime = p->vruntime;
    struct proc *min_p = p;

    for(p = p + 1; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      sum_weight += p->weight;

      if (p->vruntime < min_vruntime) {
	      min_vruntime = p->vruntime;
	      min_p = p;
	      }
    }

    min_p->timeslice = 10000 * (min_p->weight / sum_weight);
    c->proc = min_p;
    switchuvm(min_p);

    min_p->state = RUNNING;

    swtch(&(c->scheduler), min_p->context);
    switchkvm();

    c->proc = 0;



    release(&ptable.lock);

  }
}
*/

void
scheduler(void)
{
	struct proc *p;
	struct cpu *c = mycpu();
	c->proc = 0;

	for (;;) {
		sti();

		acquire(&ptable.lock);

		struct uint2 min_vruntime;
		int init = 0;
		int sum_weight = 0;
		struct proc *minp = 0;

		for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
			if (p->state != RUNNABLE) 
				continue;

			sum_weight += p->weight;

			if (!init) {
				min_vruntime = p->vruntime;
				minp = p;
				init = 1;
				}

			if (p->vruntime.high < min_vruntime.high) {
				min_vruntime = p->vruntime;
				minp = p;
				
			} else if (p->vruntime.high == min_vruntime.high) {
				if (p->vruntime.low < min_vruntime.low) {
					min_vruntime = p->vruntime;
					minp = p;
				}
				
			}
		}
		
		if (minp != 0 ) {	
			minp->timeslice = 10000 * minp->weight / sum_weight;

			c->proc = minp;
			
			switchuvm(minp);
			minp->state = RUNNING;
			
			swtch(&(c->scheduler), minp->context);
			switchkvm();

			c->proc = 0;
		}
			

		release(&ptable.lock);
		}
	}
											


// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should:ddddd
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
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
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

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

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->state == SLEEPING && p->chan == chan) {
     
	struct uint2 min_vruntime;
	uint tick1_vruntime = 1000 * 1024 / p->weight;
	int init = 0;

	struct proc *p1;

      for (p1 = ptable.proc; p1 < &ptable.proc[NPROC]; p1++) {
		if (p1->state == RUNNABLE) {
			if (!init) {
				min_vruntime = p1->vruntime;
				init = 1;
				
			}

			if (p1->vruntime.high < min_vruntime.high) {
				min_vruntime = p1->vruntime;
			} else if ((p1->vruntime.high == min_vruntime.high) && (p1->vruntime.low < min_vruntime.low)) {
				min_vruntime = p1->vruntime;
			}

		} //if      
	 } //for

	 if (init != 0) {
		 if (min_vruntime.high != 0) {
			if (min_vruntime.low >= tick1_vruntime) {
				min_vruntime.low -= tick1_vruntime;
				p->vruntime = min_vruntime;
			} else {
				min_vruntime.high -= 1;
				min_vruntime.low = 0xFFFFFFFF - tick1_vruntime + min_vruntime.low + 1; 	
			} 
			 
		} else if (min_vruntime.low < tick1_vruntime) {
			p->vruntime.high = 0;
			p->vruntime.low = 0;
			
		} else {
		min_vruntime.low -= tick1_vruntime;
		p->vruntime = min_vruntime;
			
		}
		 

	 } else {
		 p->vruntime.high = 0;
		 p->vruntime.low = 0;
	 } //else
	
	p->state = RUNNABLE;  
	} //if
	} //for
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

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
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
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }

}

int
getpname(int pid)
{
  struct proc *p;
  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->pid == pid) {
      cprintf("%s\n", p->name);
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

int
getnice(int pid)
{
	struct proc *p;
	acquire(&ptable.lock);
	for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
		if (p->pid == pid) {
			release(&ptable.lock);
			return p->priority;
		}
	}
	release(&ptable.lock);
	return -1;
}

int
setnice(int pid, int value)
{
	struct proc *p;
	acquire(&ptable.lock);
	for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
		if ((p->pid == pid) && (0<=value) && (value <= 39)) {
			p->priority = value;
			p->weight = weights[p->priority];
			release(&ptable.lock);
			return 0;
		}	 
	}
	release(&ptable.lock);
	return -1;
}



void
ps(int pid)
{
	static char *states[] = {
		   [UNUSED]    "UNUSED",
		   [EMBRYO]    "EMBRYO",
		   [SLEEPING]  "SLEEPING",
		   [RUNNABLE]  "RUNNABLE",
		   [RUNNING]   "RUNNING",
		   [ZOMBIE]    "ZOMBIE"
	 };



	struct proc *p;
	acquire(&ptable.lock);
	
	if (pid == 0) {
		char* name = str_indent("name");
		char* pid = str_indent("pid");
		char* state = str_indent("state");
		char* priority = str_indent("priority");
		char* runweight	= str_indent("runtime/weight");
		char* runtime = str_indent("runtime");
		char* vruntime = str_indent("vruntime");
		char* tick = str_indent("tick");

		cprintf("%s %s %s %s %s %s %s %s %d\n", name, pid, state, priority, runweight, runtime, vruntime, tick, ticks * 1000);
		
		for (p = ptable.proc; p < &ptable.proc[NPROC] && p -> pid != 0; p++) {
			/*
			if (p->vruntime >> 31 == 1) {
				char* pos_str = print_unsigned(p->vruntime);
				cprintf("%s %s %s %s %s %s %s\n",str_indent(p->name),int_indent(p->pid),str_indent(states[p->state]), int_indent(p->priority), int_indent(p->runtime/p->weight), int_indent(p->runtime), struct_indent(p->vruntime));

			}*/
			char* pname = str_indent(p->name);
			char* ppid = int_indent(p->pid);
			char* pstate = str_indent(states[p->state]);
			char* ppriority = int_indent(p->priority);
			char* prunweight = int_indent(p->runtime/p->weight);
			char* pruntime = int_indent(p->runtime);
			char* pvruntime = struct_indent(p->vruntime);
			 
			cprintf("%s %s %s %s %s %s %s\n", pname, ppid, pstate, ppriority, prunweight, pruntime, pvruntime);
			
			kfree(pname);
			kfree(ppid);
			kfree(pstate);
			kfree(ppriority);
			kfree(prunweight);
			kfree(pruntime);
			kfree(pvruntime);		
	
		}

		kfree(name);
		kfree(pid);
		kfree(state);
		kfree(priority);
		kfree(runweight);
		kfree(runtime);
		kfree(vruntime);
		kfree(tick);
		
		//cprintf("succeed\n");
		release(&ptable.lock);
		return;
	}
	else {
		for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
			if (pid == p->pid) {
				char* name = str_indent("name");
				char* pid = str_indent("pid");
				char* state = str_indent("state");
				char* priority = str_indent("priority");
				char* runweight = str_indent("runtime/weight");
				char* runtime = str_indent("runtime");
				char* vruntime = str_indent("vruntime");
				char* tick = str_indent("tick");

				char* pname = str_indent(p->name);
				char* ppid = int_indent(p->pid);
				char* pstate = str_indent(states[p->state]);
				char* ppriority = int_indent(p->priority);
				char* prunweight = int_indent(p->runtime/p->weight);
				char* pruntime = int_indent(p->runtime);
				char* pvruntime = struct_indent(p->vruntime);

				cprintf("%s %s %s %s %s %s %s %s %d\n",name, pid, state, priority, runweight, runtime, vruntime, tick, ticks * 1000);
				/*
				if (p->vruntime >> 31 == 1) {
					char* pos_str = print_unsigned(p->vruntime);
					cprintf("%s %s %s %s %s %s %s\n",str_indent(p->name),int_indent(p->pid),str_indent(states[p->state]), int_indent(p->priority), int_indent(p->runtime/p->weight), int_indent(p->runtime), str_indent(pos_str));
					

				} */
				
					//cprintf("%s\t %d\t %s\t %d\t %d\t %d\t %d\n",p->name,p->pid,states[p->state], p->priority, p->runtime/p->weight, p->runtime, p->vruntime);
					cprintf("%s %s %s %s %s %s %s\n", pname, ppid, pstate, ppriority, prunweight, pruntime, pvruntime);
				


				kfree(pname);
				kfree(ppid);
				kfree(pstate);
				kfree(ppriority);
				kfree(prunweight);
				kfree(pruntime);
				kfree(pvruntime);
				
				kfree(name);
				kfree(pid);
				kfree(state);
				kfree(priority);
				kfree(runweight);
				kfree(runtime);
				kfree(vruntime);
				kfree(tick);
				
				release(&ptable.lock);
				return;
			}
			
		}
		release(&ptable.lock);
		return;
	}
	
}

char* struct_indent(struct uint2 vruntime) {

	char* high_str = int_to_str(vruntime.high);
	char* low_str = int_to_str(vruntime.low);

	if (vruntime.high == 0) {
		char* result_str = str_indent(low_str);
		kfree(high_str);
		return result_str;	
		
	} else {
		int len1 = strlen(high_str);
		int len2 = strlen(low_str);
		
		char* temp_str = (char*)kalloc();

		int i,j;		
		for (i = 0; i < len2 ; i++) {
			temp_str[i] = low_str[i];
				
		}
		int k;
		for (k=0; k < 10-i;k++) {
			temp_str[i+k] = '0';	
		}
		 		
		for (j = 0; j < len1; j++) {
			temp_str[10+j] = high_str[j];
				
		}
		
		temp_str[10+j] = '\0';
		
		char* str = str_indent(temp_str);
		kfree(high_str);
		kfree(low_str);

		return str;
	}
}
/*
char* print_unsigned(unsigned int x) {
	static char pos_str[11];
	int i = 0;

	do {
		pos_str[i++] = '0' + (x % 10);
		x /= 10;

	} while (x);

	pos_str[i] = '\0';

	int start = 0;
	int end = i - 1;

	while (start < end) {
	char temp = pos_str[start];
	pos_str[start] = pos_str[end];
	pos_str[end] = temp;
	start++;
	end--;
		
	}
	
	return pos_str;
	}
*/
char* str_indent(char* index) {
	
	int size = 15;
	int str_len = strlen(index);
	
	char* result_str = (char*)kalloc();

	int padd = size - str_len;
	if (padd > 0) {
		memset(result_str, ' ', padd);
		strncpy(result_str + padd, index, str_len);
	} else {
		strncpy(result_str, index, size);
			
	}
	result_str[size] = '\0';

	if ((index[0] >= '0') && (index[0] <= '9')) {
		kfree(index);	
	}

	return result_str;

	}

char* int_indent(int num) {
	
	char* index = int_to_str(num);
	char* result_str = str_indent(index);

	return result_str;
}

char* int_to_str(int num) {
	char* str = (char*)kalloc();
	int i = 0;


	if (num == 0) {
		str[i++] = '0';
		str[i] = '\0';
		
		return str;
	}

	while (num != 0 && i < 11) {
		str[i++] = '0' + (num % 10);
		num /= 10;
	}

	str[i] = '\0';

	for (int start = 0, end = i-1; start < end; start++, end--) {
		char temp = str[start];
		str[start] = str[end];
		str[end] = temp;
		
	}
	return str;
	}

//memory free, int to str
