/*
 * Copyright (C) 2013-2019 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * This code is a complete clean re-write of the stress tool by
 * Colin Ian King <colin.king@canonical.com> and attempts to be
 * backwardly compatible with the stress tool by Amos Waterland
 * <apw@rossby.metr.ou.edu> but has more stress tests and more
 * functionality.
 *
 */
#include "stress-ng.h"

#if defined(HAVE_LIB_PTHREAD)

typedef struct {
	pthread_t pthread;
	pid_t     tid;
} pthread_info_t;

static pthread_cond_t cond;
static pthread_mutex_t mutex;
static shim_pthread_spinlock_t spinlock;
static bool thread_terminate;
static uint64_t pthread_count;
static sigset_t set;
static pthread_info_t pthreads[MAX_PTHREAD];

#endif

int stress_set_pthread_max(const char *opt)
{
	uint64_t pthread_max;

	pthread_max = get_uint64(opt);
	check_range("pthread-max", pthread_max,
		MIN_PTHREAD, MAX_PTHREAD);
	return set_setting("pthread-max", TYPE_ID_UINT64, &pthread_max);
}

#if defined(HAVE_LIB_PTHREAD)

#if defined(HAVE_GET_ROBUST_LIST) && defined(HAVE_LINUX_FUTEX_H)
static inline long sys_get_robust_list(int pid, struct robust_list_head **head_ptr, size_t *len_ptr)
{
	return syscall(__NR_get_robust_list, pid, head_ptr, len_ptr);
}
#endif

#if defined(HAVE_SET_ROBUST_LIST) && defined(HAVE_LINUX_FUTEX_H)
static inline long sys_set_robust_list(struct robust_list_head *head, size_t len)
{
	return syscall(__NR_set_robust_list, head, len);
}
#endif

/*
 *  stress_pthread_func()
 *	pthread that exits immediately
 */
static void *stress_pthread_func(void *parg)
{
	uint8_t stack[SIGSTKSZ + STACK_ALIGNMENT];
	static void *nowt = NULL;
	int ret;
	const pid_t tid = shim_gettid();
#if defined(HAVE_GET_ROBUST_LIST) && defined(HAVE_LINUX_FUTEX_H)
	struct robust_list_head *head;
	size_t len;
#endif
	const args_t *args = ((pthread_args_t *)parg)->args;

	/*
	 *  Block all signals, let controlling thread
	 *  handle these
	 */
#if !defined(__APPLE__) && !defined(__DragonFly__)
	(void)sigprocmask(SIG_BLOCK, &set, NULL);
#endif

	/*
	 *  According to POSIX.1 a thread should have
	 *  a distinct alternative signal stack.
	 *  However, we block signals in this thread
	 *  so this is probably just totally unncessary.
	 */
	(void)memset(stack, 0, sizeof(stack));
	if (stress_sigaltstack(stack, SIGSTKSZ) < 0)
		goto die;

#if defined(HAVE_GETTID)
	{
		pthread_info_t *pi = ((pthread_args_t *)parg)->data;
		pi->tid = shim_gettid();
	}
#endif

#if defined(HAVE_GET_ROBUST_LIST) && defined(HAVE_LINUX_FUTEX_H)
	/*
	 *  Check that get_robust_list() works OK
	 */
	if (sys_get_robust_list(0, &head, &len) < 0) {
		if (errno != ENOSYS) {
			pr_fail("%s: get_robust_list failed, tid=%d, errno=%d (%s)",
				args->name, tid, errno, strerror(errno));
			goto die;
		}
	} else {
#if defined(HAVE_SET_ROBUST_LIST) && defined(HAVE_LINUX_FUTEX_H)
		if (sys_set_robust_list(head, len) < 0) {
			if (errno != ENOSYS) {
				pr_fail("%s: set_robust_list failed, tid=%d, errno=%d (%s)",
					args->name, tid, errno, strerror(errno));
				goto die;
			}
		}
#endif
	}
#endif

	/*
	 *  Bump count of running threads
	 */
	ret = shim_pthread_spin_lock(&spinlock);
	if (ret) {
		pr_fail("%s: pthread_spin_lock failed, tid=%d, errno=%d (%s)",
			args->name, tid, ret, strerror(ret));
		goto die;
	}
	pthread_count++;
	ret = shim_pthread_spin_unlock(&spinlock);
	if (ret) {
		pr_fail("%s: pthread_spin_unlock failed, tid=%d, errno=%d (%s)",
			args->name, tid, ret, strerror(ret));
		goto die;
	}

	/*
	 *  Wait for controlling thread to
	 *  indicate it is time to die
	 */
	ret = pthread_mutex_lock(&mutex);
	if (ret) {
		pr_fail("%s: pthread_mutex_lock failed, tid=%d, errno=%d (%s)",
			args->name, tid, ret, strerror(ret));
		goto die;
	}
	while (!thread_terminate) {
		ret = pthread_cond_wait(&cond, &mutex);
		if (ret) {
			pr_fail("%s: pthread_cond_wait failed, tid=%d, errno=%d (%s)",
				args->name, tid, ret, strerror(ret));
			break;
		}
		(void)shim_sched_yield();
	}
	ret = pthread_mutex_unlock(&mutex);
	if (ret)
		pr_fail("%s: pthread_mutex_unlock failed, tid=%d, errno=%d (%s)",
			args->name, tid, ret, strerror(ret));

#if defined(HAVE_SETNS)
	{
		int fd;

		fd = open("/proc/self/ns/uts", O_RDONLY);
		if (fd >= 0) {
			/*
			 *  Capabilities have been dropped
			 *  so this will always fail, but
			 *  lets exercise it anyhow.
			 */
			(void)setns(fd, 0);
			(void)close(fd);
		}
	}
#endif
die:
	return &nowt;
}

/*
 *  stress_pthread()
 *	stress by creating pthreads
 */
static int stress_pthread(const args_t *args)
{
	bool ok = true;
	uint64_t limited = 0, attempted = 0;
	uint64_t pthread_max = DEFAULT_PTHREAD;
	int ret;
	pthread_args_t pargs = { args, NULL };

#if defined(SIGUSR2)
	if (stress_sighandler(args->name, SIGUSR2, SIG_IGN, NULL) < 0)
		return EXIT_FAILURE;
#endif

	if (!get_setting("pthread-max", &pthread_max)) {
		if (g_opt_flags & OPT_FLAGS_MAXIMIZE)
			pthread_max = MAX_PTHREAD;
		if (g_opt_flags & OPT_FLAGS_MINIMIZE)
			pthread_max = MIN_PTHREAD;
	}

	ret = pthread_cond_init(&cond, NULL);
	if (ret) {
		pr_fail("%s pthread_cond_init failed, errno=%d (%s)",
			args->name, ret, strerror(ret));
		return EXIT_FAILURE;
	}
	ret = shim_pthread_spin_init(&spinlock, SHIM_PTHREAD_PROCESS_SHARED);
	if (ret) {
		pr_fail("%s pthread_spin_init failed, errno=%d (%s)",
			args->name, ret, strerror(ret));
		return EXIT_FAILURE;
	}
	ret = pthread_mutex_init(&mutex, NULL);
	if (ret) {
		pr_fail("%s pthread_mutex_init failed, errno=%d (%s)",
			args->name, ret, strerror(ret));
		return EXIT_FAILURE;
	}

	(void)sigfillset(&set);
	do {
		uint64_t i, j;

		thread_terminate = false;
		pthread_count = 0;

		(void)memset(&pthreads, 0, sizeof(pthreads));

		for (i = 0; (i < pthread_max) && (!args->max_ops || get_counter(args) < args->max_ops); i++) {
			pargs.data = (void *)&pthreads[i];

			ret = pthread_create(&pthreads[i].pthread, NULL,
				stress_pthread_func, (void *)&pargs);
			if (ret) {
				/* Out of resources, don't try any more */
				if (ret == EAGAIN) {
					limited++;
					break;
				}
				/* Something really unexpected */
				pr_fail("%s pthread_create failed, errno=%d (%s)",
					args->name, ret, strerror(ret));
				ok = false;
				break;
			}
			inc_counter(args);
			if (!g_keep_stressing_flag)
				break;
		}
		attempted++;

		/*
		 *  Wait until they are all started or
		 *  we get bored waiting..
		 */
		for (j = 0; j < 1000; j++) {
			bool all_running = false;

			ret = pthread_mutex_lock(&mutex);
			if (ret) {
				pr_fail("%s pthread_mutex_lock failed (parent), errno=%d (%s)",
					args->name, ret, strerror(ret));
				ok = false;
				goto reap;
			}
			all_running = (pthread_count == i);
			ret = pthread_mutex_unlock(&mutex);
			if (ret) {
				pr_fail("%s pthread_mutex_unlock failed (parent), errno=%d (%s)",
					args->name, ret, strerror(ret));
				ok = false;
				goto reap;
			}

			if (all_running)
				break;
		}

		ret = pthread_mutex_lock(&mutex);
		if (ret) {
			pr_fail("%s pthread_mutex_lock failed (parent), errno=%d (%s)",
				args->name, ret, strerror(ret));
			ok = false;
			goto reap;
		}
#if defined(HAVE_TGKILL) && defined(SIGUSR2)
		for (j = 0; j < i; j++) {
			if (pthreads[j].tid)
				(void)syscall(__NR_tgkill, args->pid, pthreads[j].tid, SIGUSR2);
		}
#endif
		thread_terminate = true;
		ret = pthread_cond_broadcast(&cond);
		if (ret) {
			pr_fail("%s pthread_cond_broadcast failed (parent), errno=%d (%s)",
				args->name, ret, strerror(ret));
			ok = false;
			/* fall through and unlock */
		}
		ret = pthread_mutex_unlock(&mutex);
		if (ret) {
			pr_fail("%s pthread_mutex_unlock failed (parent), errno=%d (%s)",
				args->name, ret, strerror(ret));
			ok = false;
		}

reap:
		for (j = 0; j < i; j++) {
			ret = pthread_join(pthreads[j].pthread, NULL);
			if ((ret) && (ret != ESRCH)) {
				pr_fail("%s pthread_join failed (parent), errno=%d (%s)",
					args->name, ret, strerror(ret));
				ok = false;
			}
		}
	} while (ok && keep_stressing());

	if (limited) {
		pr_inf("%s: %.2f%% of iterations could not reach "
			"requested %" PRIu64 " threads (instance %"
			PRIu32 ")\n",
			args->name,
			100.0 * (double)limited / (double)attempted,
			pthread_max, args->instance);
	}

	(void)pthread_cond_destroy(&cond);
	(void)pthread_mutex_destroy(&mutex);
	(void)shim_pthread_spin_destroy(&spinlock);

	return EXIT_SUCCESS;
}

stressor_info_t stress_pthread_info = {
	.stressor = stress_pthread,
	.class = CLASS_SCHEDULER | CLASS_OS
};
#else
stressor_info_t stress_pthread_info = {
	.stressor = stress_not_implemented,
	.class = CLASS_SCHEDULER | CLASS_OS
};
#endif
