/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Process ID management.
 */

#include <types.h>
#include <kern/errno.h>
#include <kern/wait.h>
#include <limits.h>
#include <lib.h>
#include <array.h>
#include <clock.h>
#include <thread.h>
#include <current.h>
#include <synch.h>
#include <pid.h>
#include <copyinout.h>

/*
 * Structure for holding PID and return data for a thread.
 *
 * If pi_ppid is INVALID_PID, the parent has gone away and will not be
 * waiting. If pi_ppid is INVALID_PID and pi_exited is true, the
 * structure can be freed.
 */
struct pidinfo {
	pid_t pi_pid;			// process id of this thread
	pid_t pi_ppid;			// process id of parent thread
	volatile bool pi_exited;	// true if thread has exited
	int pi_exitstatus;		// status (only valid if exited)
	struct cv *pi_cv;		// use to wait for thread exit
	//struct cv *pi_cv_stop;	// use to wait for SIGCONT if SIGSTOP is set
	volatile bool pi_detached; //a disowned thread means parent id == INVALID, detached means non joinable
	int pi_flag;			// flag the pid
	int num_threads_joined; //for pid_join to allow multiple threads to join it

};


/*
 * Global pid and exit data.
 *
 * The process table is an el-cheapo hash table. It's indexed by
 * (pid % PROCS_MAX), and only allows one process per slot. If a
 * new pid allocation would cause a hash collision, we just don't
 * use that pid.
 */
static struct lock *pidlock;		// lock for global exit data
static struct pidinfo *pidinfo[PROCS_MAX]; // actual pid info
static pid_t nextpid;			// next candidate pid
static int nprocs;			// number of allocated pids



/*
 * Create a pidinfo structure for the specified pid.
 */
static
struct pidinfo *
pidinfo_create(pid_t pid, pid_t ppid)
{
	struct pidinfo *pi;

	KASSERT(pid != INVALID_PID);

	pi = kmalloc(sizeof(struct pidinfo));
	if (pi==NULL) {
		return NULL;
	}

	pi->pi_cv = cv_create("pidinfo cv");
	if (pi->pi_cv == NULL) {
		kfree(pi);
		return NULL;
	}

	/* Create info to stop*/
	/*pi->pi_cv_stop = cv_create("pidinfo cv for stop");
	if (pi->pi_cv_stop == NULL) {
		kfree(pi);
		return NULL;
	}*/
	pi->pi_pid = pid;
	pi->pi_ppid = ppid;
	pi->pi_exited = false;
	pi->pi_detached = false;
	pi->pi_exitstatus = 0xbaad;  /* Recognizably invalid value */
	pi->num_threads_joined = 0;
	pi->pi_flag = 0;
	

	return pi;
}

/*
 * Clean up a pidinfo structure.
 */
static
void
pidinfo_destroy(struct pidinfo *pi)
{
	KASSERT(pi->pi_exited == true);
	KASSERT(pi->pi_ppid == INVALID_PID);
	cv_destroy(pi->pi_cv);
	kfree(pi);
}

////////////////////////////////////////////////////////////

/*
 * pid_bootstrap: initialize.
 */
void
pid_bootstrap(void)
{
	int i;

	pidlock = lock_create("pidlock");
	if (pidlock == NULL) {
		panic("Out of memory creating pid lock\n");
	}

	/* not really necessary - should start zeroed */
	for (i=0; i<PROCS_MAX; i++) {
		pidinfo[i] = NULL;
	}

	pidinfo[BOOTUP_PID] = pidinfo_create(BOOTUP_PID, INVALID_PID);
	if (pidinfo[BOOTUP_PID]==NULL) {
		panic("Out of memory creating bootup pid data\n");
	}

	nextpid = PID_MIN;
	nprocs = 1;
}

/*
 * pi_get: look up a pidinfo in the process table.
 */
static
struct pidinfo *
pi_get(pid_t pid)
{
	struct pidinfo *pi;

	KASSERT(pid>=0);
	KASSERT(pid != INVALID_PID);
	KASSERT(lock_do_i_hold(pidlock));

	pi = pidinfo[pid % PROCS_MAX];
	if (pi==NULL) {
		return NULL;
	}
	if (pi->pi_pid != pid) {
		return NULL;
	}
	return pi;
}

/*
 * pi_put: insert a new pidinfo in the process table. The right slot
 * must be empty.
 */
static
void
pi_put(pid_t pid, struct pidinfo *pi)
{
	KASSERT(lock_do_i_hold(pidlock));

	KASSERT(pid != INVALID_PID);

	KASSERT(pidinfo[pid % PROCS_MAX] == NULL);
	pidinfo[pid % PROCS_MAX] = pi;
	nprocs++;
}

/*
 * pi_drop: remove a pidinfo structure from the process table and free
 * it. It should reflect a process that has already exited and been
 * waited for.
 */
static
void
pi_drop(pid_t pid)
{
	struct pidinfo *pi;

	KASSERT(lock_do_i_hold(pidlock));

	pi = pidinfo[pid % PROCS_MAX];
	KASSERT(pi != NULL);
	KASSERT(pi->pi_pid == pid);

	pidinfo_destroy(pi);
	pidinfo[pid % PROCS_MAX] = NULL;
	nprocs--;
}

////////////////////////////////////////////////////////////

/*
 * Helper function for pid_alloc.
 */
static
void
inc_nextpid(void)
{
	KASSERT(lock_do_i_hold(pidlock));

	nextpid++;
	if (nextpid > PID_MAX) {
		nextpid = PID_MIN;
	}
}

/*
 * pid_alloc: allocate a process id.
 */
int
pid_alloc(pid_t *retval)
{
	struct pidinfo *pi;
	pid_t pid;
	int count;

	KASSERT(curthread->t_pid != INVALID_PID);

	/* lock the table */
	lock_acquire(pidlock);

	if (nprocs == PROCS_MAX) {
		lock_release(pidlock);
		return EAGAIN;
	}

	/*
	 * The above test guarantees that this loop terminates, unless
	 * our nprocs count is off. Even so, assert we aren't looping
	 * forever.
	 */
	count = 0;
	while (pidinfo[nextpid % PROCS_MAX] != NULL) {

		/* avoid various boundary cases by allowing extra loops */
		KASSERT(count < PROCS_MAX*2+5);
		count++;

		inc_nextpid();
	}

	pid = nextpid;

	pi = pidinfo_create(pid, curthread->t_pid);
	if (pi==NULL) {
		lock_release(pidlock);
		return ENOMEM;
	}

	pi_put(pid, pi);

	inc_nextpid();

	lock_release(pidlock);

	*retval = pid;
	return 0;
}

/*
 * pid_unalloc - unallocate a process id (allocated with pid_alloc) that
 * hasn't run yet.
 */
void
pid_unalloc(pid_t theirpid)
{
	struct pidinfo *them;

	KASSERT(theirpid >= PID_MIN && theirpid <= PID_MAX);

	lock_acquire(pidlock);

	them = pi_get(theirpid);
	KASSERT(them != NULL);
	KASSERT(them->pi_exited == false);
	KASSERT(them->pi_ppid == curthread->t_pid);

	/* keep pidinfo_destroy from complaining */
	them->pi_exitstatus = 0xdead;
	them->pi_exited = true;
	them->pi_ppid = INVALID_PID;

	pi_drop(theirpid);

	lock_release(pidlock);
}

/*
 * pid_detach - disavows interest in the child thread's exit status, so 
 * it can be freed as soon as it exits. May only be called by the
 * parent thread.
 */
int
pid_detach(pid_t childpid)
{
	// Check is childpid is invalid
	if (childpid == INVALID_PID || childpid == BOOTUP_PID) {
		return EINVAL;
	}

	struct pidinfo *pi;
	lock_acquire(pidlock);
	pi = pi_get(childpid);

	// Check if the pid exists
	if (pi == NULL) {
		return ESRCH;
	}
	// or if detached
	if (pi->pi_detached) {
		return EINVAL;
	}
	// or invalid
	if (pi->pi_ppid == INVALID_PID) {
		return EINVAL;
	}
	// or parent is not calling
	if (curthread->t_pid != pi->pi_ppid) {
		return EINVAL;
	}
	//or has existing joined threads
	if (pi->num_threads_joined > 0) {
		return EINVAL;
	}

    // detach if not invalid
	pi->pi_detached = true;
	// check if child exited
	if(pi->pi_exited) {
		pi_drop(childpid);
	}

	lock_release(pidlock);	
	return 0;
}

/*
 * pid_exit 
 *  - sets the exit status of this thread (i.e. curthread). 
 *  - disowns children. 
 *  - if dodetach is true, children are also detached. 
 *  - wakes any thread waiting for the curthread to exit. 
 *  - frees the PID and exit status if the curthread has been detached. 
 *  - must be called only if the thread has had a pid assigned.
 */
void
pid_exit(int status, bool dodetach)
{
	struct pidinfo *my_pi;

	lock_acquire(pidlock);

	/* get the pidinfo and set its exit status to true, and set the exit code */
	my_pi = pi_get(curthread->t_pid);
	KASSERT(my_pi != NULL);
	my_pi->pi_exitstatus = status;
	my_pi->pi_exited = true;

	// Search through pids for the children and detach them
	for (int i = 0; i < PROCS_MAX; i++) {
		// check if child pid matches parent and not NULL
		if (pidinfo[i] != NULL && pidinfo[i]->pi_ppid == curthread->t_pid) {
			pidinfo[i]->pi_ppid = INVALID_PID; //disown the child
			pidinfo[i]->pi_detached = dodetach; //detach the child
			// drop child if exited
			if (pidinfo[i]->pi_exited) {
				pi_drop(pidinfo[i]->pi_pid);
			}
		}
	}

	// if not invalid but detached, drop it
	if (my_pi->pi_detached) {
		my_pi->pi_ppid = INVALID_PID;
		pi_drop(curthread->t_pid);
	} else {
		// if not detached, wake up waiting threads with signal
		my_pi->pi_ppid = INVALID_PID;
		cv_signal(my_pi->pi_cv, pidlock);
	}

	lock_release(pidlock);
}

/*
 * pid_join - returns the exit status of the thread associated with
 * targetpid as soon as it is available. If the thread has not yet 
 * exited, curthread waits unless the flag WNOHANG is sent. 
 *`
 */
int
pid_join(pid_t targetpid, int *status, int flags)
{
	// Check is target pid is invalid
	if(targetpid == INVALID_PID || targetpid == BOOTUP_PID) {
		return -EINVAL;
	}

	struct pidinfo *pi;
	lock_acquire(pidlock);
	// get the target pidinfo
	pi = pi_get(targetpid);

	// check if target exists
	if (pi == NULL) {
		lock_release(pidlock);
		return -ESRCH;
	}
	// or if deatched
	if (pi->pi_detached) {
		return -EINVAL;
	}
	// or if target is trying to join itself (deadlock)
	if (pi->pi_pid == curthread->t_pid) {
		lock_release(pidlock);
		return -EDEADLK;
	}
	// or if target already exited
	if (pi->pi_exited) {
	    // change status if necessary
		if (status != NULL) {
			*status = pi->pi_exitstatus;
		}
        // drop pidinfo and reduce number of joined threads
		pi->num_threads_joined--;
		// check for any more joined threads and drop when none left
		if (!pi->num_threads_joined) {
			pi_drop(targetpid);
		}
		lock_release(pidlock);
		return curthread->t_pid;
	} else {
	    // deal with WNOHANG
		if ((flags & WNOHANG) == WNOHANG) {
			lock_release(pidlock);
			return 0;
		}
		// increase joined threads since we wait
		pi->num_threads_joined++;
		cv_wait(pi->pi_cv, pidlock);

        // change status if necessary
		if (status != NULL) {
			*status = pi->pi_exitstatus;
		}
        // drop pidinfo and reduce number of joined threads
		pi->num_threads_joined--;
		// check for any more joined threads and drop when none left
		if (!pi->num_threads_joined) {
			pi_drop(targetpid);
		}
		lock_release(pidlock);
		return curthread->t_pid;
	}
}

//additional monitoring tools for pid

/*
 * pid_set_flag:
 * Set signal sig to process flag. On success, 0 is returned. On error,
 * return errno.
 */
int
pid_set_flag(pid_t t_pid, int flag){

	lock_acquire(pidlock);

    // check if valid pid
	if (t_pid == INVALID_PID || t_pid > PID_MAX || t_pid < PID_MIN){
		lock_release(pidlock);
		return EINVAL;
	}

    // get pidinfo
	struct pidinfo* cur_info = pi_get(t_pid);
	// check if pid exists
	if (cur_info == NULL) {
		lock_release(pidlock);
		return ESRCH;
	}
    // set flag and return
	cur_info->pi_flag = flag;
	lock_release(pidlock);
	return 0;
}

/*
 * pid_get_flag: Return signal flag of the process specified by pid. On error,
 * a negative errno is returned.
 */
int
pid_get_flag(pid_t t_pid){

	lock_acquire(pidlock);

    // check if valid pid
	if (t_pid == INVALID_PID || t_pid > PID_MAX || t_pid < PID_MIN){
		lock_release(pidlock);
		return EINVAL;
	}

    // get pidinfo
	struct pidinfo* cur_info = pi_get(t_pid);
	// check if pid exists
	if (cur_info == NULL) {
		lock_release(pidlock);
		return ESRCH;
	}
    // set flag and return
	int fvalue = cur_info->pi_flag;
	lock_release(pidlock);
	return fvalue;
}

/*
 * Put a process into sleep state - waiting
 */
int
pid_sleep(pid_t t_pid){

	lock_acquire(pidlock);

	if (t_pid == INVALID_PID || t_pid > PID_MAX || t_pid < PID_MIN){
		//check for invalid signal
		lock_release(pidlock);
		return EINVAL;
	}

	struct pidinfo* cur_info = pi_get(t_pid);

	if (cur_info == NULL) {
		//check if pid exist
		lock_release(pidlock);
		return ESRCH;
	}

	cv_wait(cur_info->pi_cv, pidlock);
	lock_release(pidlock);
	return 0;
}

/*
 * Signal a process out of sleep
 */
int
pid_wakeup(pid_t t_pid){

	lock_acquire(pidlock);

	if (t_pid == INVALID_PID || t_pid > PID_MAX || t_pid < PID_MIN){
		//check for invalid signal
		lock_release(pidlock);
		return EINVAL;
	}

	struct pidinfo* cur_info = pi_get(t_pid);

	if (cur_info == NULL) {
		//check if pid exist
		lock_release(pidlock);
		return ESRCH;
	}

	cv_signal(cur_info->pi_cv, pidlock);
	lock_release(pidlock);
	return 0;
}

