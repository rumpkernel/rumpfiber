/*
 * Copyright (c) 2014 Justin Cormack.  All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/* Based partly on the following code from Xen Minios */

/* 
 ****************************************************************************
 * (C) 2005 - Grzegorz Milos - Intel Research Cambridge
 ****************************************************************************
 *
 *        File: sched.c
 *      Author: Grzegorz Milos
 *     Changes: Robert Kaiser
 *              
 *        Date: Aug 2005
 * 
 * Environment: Xen Minimal OS
 * Description: simple scheduler for Mini-Os
 *
 * The scheduler is non-preemptive (cooperative), and schedules according 
 * to Round Robin algorithm.
 *
 ****************************************************************************
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR 
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE 
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER 
 * DEALINGS IN THE SOFTWARE.
 */

#include <stdlib.h>
#include <stddef.h>
#include <sys/mman.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <ucontext.h>

#include "rumpfiber_thread.h"

#define printk(...) fprintf(stderr, __VA_ARGS__)

TAILQ_HEAD(thread_list, thread);

static struct thread_list exited_threads = TAILQ_HEAD_INITIALIZER(exited_threads);
static struct thread_list thread_list = TAILQ_HEAD_INITIALIZER(thread_list);
static int threads_started;
static struct thread *current_thread = NULL;
struct thread *idle_thread = NULL;

static inline struct thread *
get_current(void)
{

	return current_thread;
}

static inline int64_t
tsms(struct timespec *ts)
{

	return (ts->tv_sec * 1000LL) + (ts->tv_nsec / 1000000LL);
}

static inline int64_t
now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return tsms(&ts);
}

void
schedule(void)
{
	struct thread *prev, *next, *thread, *tmp;
	int64_t tm, wakeup;
	struct timespec sl;

	prev = get_current();

	do {
		tm = now();	
		wakeup = tm + 1000; /* wake up in 1s max */
		next = NULL;
		TAILQ_FOREACH_SAFE(thread, &thread_list, thread_list, tmp) {
			if (! is_runnable(thread) && thread->wakeup_time >= 0LL) {
				if (thread->wakeup_time <= tm)
					wake(thread);
				else if (thread->wakeup_time < wakeup)
					wakeup = thread->wakeup_time;
			}
			if (is_runnable(thread)) {
				next = thread;
				/* Put this thread on the end of the list */
				TAILQ_REMOVE(&thread_list, thread, thread_list);
				TAILQ_INSERT_TAIL(&thread_list, thread, thread_list);
				break;
			}
		}
		if (next)
			break;
		sl.tv_sec = (wakeup - tm) / 1000;
		sl.tv_nsec = ((wakeup - tm) - 1000 * sl.tv_sec) * 1000000;
		clock_nanosleep(CLOCK_MONOTONIC, 0, &sl, NULL);
	} while (1);

	if (prev != next)
		switch_threads(prev, next);

	TAILQ_FOREACH_SAFE(thread, &exited_threads, thread_list, tmp) {
		if (thread != prev) {
			TAILQ_REMOVE(&exited_threads, thread, thread_list);
			munmap(thread->ctx.uc_stack.ss_sp, STACKSIZE);
			free(thread);
		}
	}
}

/* may have to do bounce function to call, if args to makecontext are ints */
/* TODO see notes in rumpuser_thread_create, have flags here */
struct thread *
create_thread(const char *name, void (*f)(void *), void *data)
{
	struct thread *thread = calloc(1, sizeof(struct thread));
	char *stack;

	getcontext(&thread->ctx);
	stack = mmap(NULL, STACKSIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (stack == MAP_FAILED) {
		return NULL;
	}
	thread->ctx.uc_stack.ss_sp = stack;
	thread->ctx.uc_stack.ss_size = STACKSIZE;
	thread->ctx.uc_stack.ss_flags = 0;
	thread->ctx.uc_link = NULL; /* TODO may link to main thread */
	makecontext(&thread->ctx, (void (*)(void))f, 1, data);

	/* Not runnable, not exited, not sleeping */
	thread->flags = 0;
	thread->wakeup_time = -1LL;
	thread->lwp = NULL;
	set_runnable(thread);
	TAILQ_INSERT_TAIL(&thread_list, thread, thread_list);

	return thread;
}

void
switch_threads(struct thread *prev, struct thread *next)
{
	int ret;

	current_thread = next;
	ret = swapcontext(&prev->ctx, &next->ctx);
	if (ret < 0) {
		printk("swapcontext failed: %s\n", strerror(errno));
		abort();
	}
}

struct join_waiter {
    struct thread *jw_thread;
    struct thread *jw_wanted;
    TAILQ_ENTRY(join_waiter) jw_entries;
};
static TAILQ_HEAD(, join_waiter) joinwq = TAILQ_HEAD_INITIALIZER(joinwq);

void
exit_thread(void)
{
	struct thread *thread = get_current();
	struct join_waiter *jw_iter;

	/* if joinable, gate until we are allowed to exit */
	while (thread->flags & THREAD_MUSTJOIN) {
		thread->flags |= THREAD_JOINED;

		/* see if the joiner is already there */
		TAILQ_FOREACH(jw_iter, &joinwq, jw_entries) {
			if (jw_iter->jw_wanted == thread) {
				wake(jw_iter->jw_thread);
				break;
			}
		}
		block(thread);
		schedule();
	}

	/* Remove from the thread list */
	TAILQ_REMOVE(&thread_list, thread, thread_list);
	clear_runnable(thread);
	/* Put onto exited list */
	TAILQ_INSERT_HEAD(&exited_threads, thread, thread_list);

	/* Schedule will free the resources */
	while (1) {
		schedule();
		printk("schedule() returned!  Trying again\n");
	}
}

void
join_thread(struct thread *joinable)
{
	struct join_waiter jw;
	struct thread *thread = get_current();

	//ASSERT(joinable->flags & THREAD_MUSTJOIN); /* XXX make sure defined assert */

	/* wait for exiting thread to hit thread_exit() */
	while (! (joinable->flags & THREAD_JOINED)) {

		jw.jw_thread = thread;
		jw.jw_wanted = joinable;
		TAILQ_INSERT_TAIL(&joinwq, &jw, jw_entries);
		block(thread);
		schedule();
		TAILQ_REMOVE(&joinwq, &jw, jw_entries);
	}

	/* signal exiting thread that we have seen it and it may now exit */
	// ASSERT(joinable->flags & THREAD_JOINED); /* XXX */
	joinable->flags &= ~THREAD_MUSTJOIN;

	wake(joinable);
}

void msleep(uint64_t millisecs)
{
	struct thread *thread = get_current();

	thread->wakeup_time = now() + millisecs;
	clear_runnable(thread);
	schedule();
}

void abssleep(uint64_t millisecs)
{
	struct thread *thread = get_current();

	thread->wakeup_time = millisecs;
	clear_runnable(thread);
	schedule();
}

void wake(struct thread *thread)
{

	thread->wakeup_time = 0LL;
	set_runnable(thread);
}

void block(struct thread *thread)
{

	thread->wakeup_time = 0LL;
	clear_runnable(thread);
}

void idle_thread_fn(void *unused)
{

	threads_started = 1; /*XXX not used elsewhere */
	while (1) {
		block(get_current());
		schedule();
	}
}

void
run_idle_thread(void)
{

	current_thread = idle_thread;
	setcontext(&idle_thread->ctx);
}

void
init_sched(void)
{

	idle_thread = create_thread("Idle", idle_thread_fn, NULL);
	if (! idle_thread) {
		printk("failed to create idle thread\n");
		exit(1);
	}
}

