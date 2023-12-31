#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"

int sys_fork(void)
{
	return fork();
}

int sys_exit(void)
{
	exit();
	return 0; // not reached
}

int sys_wait(void)
{
	return wait();
}

int sys_kill(void)
{
	int pid;

	if (argint(0, &pid) < 0)
		return -1;
	return kill(pid);
}

int sys_getpid(void)
{
	return myproc()->pid;
}

int sys_sbrk(void)
{
	int addr;
	int n;

	if (argint(0, &n) < 0)
		return -1;
	addr = myproc()->sz;
	if (growproc(n) < 0)
		return -1;
	return addr;
}

int sys_sleep(void)
{
	int n;
	uint ticks0;

	if (argint(0, &n) < 0)
		return -1;
	acquire(&tickslock);
	ticks0 = ticks;
	while (ticks - ticks0 < n)
	{
		if (myproc()->killed)
		{
			release(&tickslock);
			return -1;
		}
		sleep(&ticks, &tickslock);
	}
	release(&tickslock);
	return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int sys_uptime(void)
{
	uint xticks;

	acquire(&tickslock);
	xticks = ticks;
	release(&tickslock);
	return xticks;
}

// this function returns time in seconds since process
// created.
uint sys_lifetime(void){
	uint xticks;

	acquire(&tickslock);
	xticks = ticks;
	release(&tickslock);
	
  	struct proc *my_proc = myproc(); // Get the current process

	cprintf("[sys_lifetime] Current time  is: %d this app creation time is: %d\n",xticks,my_proc->xticks);
	return (xticks-my_proc->xticks)/100;

}



void sys_print_num_syscalls(){
  // struct cpu *c;
  cprintf("total uumber of systemcalls: %d",shared_syscall_num);
  for (int i =0; i < ncpu; i++) {
    if (cpus[i].started)
    {
      cprintf("cpu %d got: %d numbers of syscalss",cpus[i].apicid,cpus[i].num_sys_calls);  
    }
  }
}