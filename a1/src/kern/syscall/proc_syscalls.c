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
#include <kern/wait.h>
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

/*
 * sys_getpid
 * Returns the process id of the current process.
 */
void
sys_getpid(pid_t *retval)
{
	*retval = curthread->t_pid;
}

/*
 * sys_waitpid
 * Wait for the process "pid" to exit
 * Save exit status of process pid to integer pointer "status"
 * If "option" is WNOHANG and the process "pid" has not yet exited,
 * return 0. On success, return 0. On error, return proper errno.
 */
int
sys_waitpid(pid_t pid, int *status, int opt, pid_t *retval)
{
	if (pid == INVALID_PID) {
		return ESRCH;
	}
	
	// If the status argument was an invalid pointer
	if (status == NULL) {
		return EFAULT;
	}

	// If the options argument requested invalid or unsupported options
	if (opt != 0 && opt != WNOHANG) {
		return EINVAL;
	}

	// If given pid is a child of current thread
	if (pid == curthread->t_pid) {
		return ECHILD;
	}

 	if (opt == WNOHANG){
 		*retval = 0;
 	} else {
 		*retval = pid;
 	}
 	
	int err = pid_join(pid, status, opt);

 	//pid_join fails
 	if (err < 0) {
 		*retval = -err;
 		return -1;
 	} else {
 		return 0;
 	}
}


/*
 * sys_kill
 * Send signal sig to process pid. Validate signal and its implementation. On
 * success, 0 is returned. On error, return errno.
 */
int
sys_kill(pid_t pid, int sig)
{
	// Validate signal sig
	if (sig < 1 || sig > 31) {
		return EINVAL;
	}

	// Check the implementation of sig
	if (sig != SIGHUP && sig != SIGINT && sig != SIGKILL && sig != SIGTERM && sig !=
			SIGSTOP && sig != SIGCONT && sig != SIGWINCH && sig != SIGINFO) {
		return EUNIMP;
	} else if (pid == INVALID_PID) {
	    return ESRCH;
	}
	
	int err = pid_set_flag(pid, sig);
	if (err < 0) {
		return -1;
	} else if (sig == SIGCONT) {
	    pid_wakeup(pid);
	}
	return 0;
}
