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
#include <synch.h>

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
 */
void
sys_getpid(pid_t *retval)
{
    // get current thread pid
    *retval=curthread->t_pid;
}

/*
 * sys_waitpid
 */
int
sys_waitpid(pid_t pid, int *status, int opt, pid_t *retval)
{
    // return if pid does not exist
    if (pid_valid(pid)!=0) {
        return ESRCH;
    }
    // return if status pointer is invalid
    if (!status) {
        return EFAULT;
    }
    // return if opt argument is invalid
    if (opt!=0 && opt!=WNOHANG) {
        return EINVAL;
    }
    // return if pid is child of current thread
    if (!pid_parent(curthread->t_pid, pid)) {
        return ECHILD;
    }
    // wait for exit status of thread in status
	*retval = pid_join(pid, status, opt);
	// return error
	if (*retval<0){
		return -(*retval);
	}
    // success
    return 0;
}

/*
 * sys_kill
 */
int
sys_kill(pid_t pid, int sig)
{
    // Validate signal sig
	if (sig<0 || sig>32) {
		return EINVAL;
	}

	int err = pid_set_flag(pid, sig);
	if (err) {
		return err;
	}
	return 0;
}

