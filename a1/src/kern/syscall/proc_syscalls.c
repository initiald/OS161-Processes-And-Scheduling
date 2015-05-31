/*
 * Process-related syscalls.
 * New for ASST1.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <current.h>
#include <pid.h>
#include <machine/trapframe.h>
#include <syscall.h>
#include <limits.h>
#include <kern/wait.h> 
#include <copyinout.h>
#include <signal.h>


/*
 * sys_fork
 * 
 * create a new process, which begins executing in md_forkentry().
 */
int
sys_fork(struct trapframe *tf, pid_t *retval)
{
	struct trapframe *ntf; /* new trapframe, copy of tf */
	int result;

	/*
	 * Copy the trapframe to the heap, because we might return to
	 * userlevel and make another syscall (changing the trapframe)
	 * before the child runs. The child will free the copy.
	 */

	ntf = kmalloc(sizeof(struct trapframe));
	if (ntf==NULL) {
		return ENOMEM;
	}
	*ntf = *tf; /* copy the trapframe */

	result = thread_fork(curthread->t_name, enter_forked_process, 
			     ntf, 0, retval);
	if (result) {
		kfree(ntf);
		return result;
	}

	return 0;
}

int
sys_getpid(pid_t *retval)
{
	*retval = curthread->t_pid;
	return 0;
}

int
sys_waitpid(pid_t *retval, pid_t pid, int *status, int options)
{

	// options argument requested is invalid or unsupported.
	if (options != 0 && options != WNOHANG)
		return EINVAL;

	if (pid == INVALID_PID){
		return EINVAL;

	}

	if (pid_valid(pid) != 0)
		return ESRCH;

	if (status == NULL)
		return EFAULT;

	if (pid_isparent(pid))
		return ECHILD;

	if (status == (void *)0x80000000 || status == (void *)0x40000000) {
		return EFAULT;
	}

	// kprintf("status address: %p \n", status);
    int stat;
    copyin((userptr_t)status, &stat, sizeof(int));
    pid_t value;
    value = pid_join(pid, &stat, options);
    // kprintf("stat: %d \n", stat);
    copyout(&stat, (userptr_t)status, sizeof(int));
    // kprintf("status: %d \n", *status);

    if (options == WNOHANG) {
        KASSERT(value == 0);
		return 0;
    }


	if (value >= 0){
		*retval = pid;
		return 0;
	}



	*retval = -value;
	return -1;
}

int
sys_kill(pid_t pid, int sig)
{

	if (sig >= 32 || sig < 0){
		return EINVAL;
	}

	/* Signal parsing. */
	switch (sig){
		case SIGHUP: // implemented
		case SIGINT:
		case SIGKILL:
		case SIGTERM:
		case SIGSTOP:
		case SIGCONT:
		case SIGWINCH:
		case SIGINFO:
		case 0:
			break;

		default: // unimplemented
			return EUNIMP;
	}

	if (pid_valid(pid) != 0)
		return ESRCH;

	if ((pid_set_flag(pid, sig)) < 0)
		return -1;
	return 0;
}
