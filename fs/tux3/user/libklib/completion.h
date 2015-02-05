#ifndef LIBKLIB_COMPLETION_H
#define LIBKLIB_COMPLETION_H

#include <libklib/wait.h>
#include <linux/errno.h>

struct completion {
	unsigned int done;
	wait_queue_head_t wait;
};

#define COMPLETION_INITIALIZER(work) \
	{ 0, __WAIT_QUEUE_HEAD_INITIALIZER((work).wait) }

#define COMPLETION_INITIALIZER_ONSTACK(work) \
	({ init_completion(&work); work; })

#define DECLARE_COMPLETION(work) \
	struct completion work = COMPLETION_INITIALIZER(work)

#define DECLARE_COMPLETION_ONSTACK(work) DECLARE_COMPLETION(work)

static inline void init_completion(struct completion *x)
{
	x->done = 0;
	init_waitqueue_head(&x->wait);
}

static inline void reinit_completion(struct completion *x)
{
	x->done = 0;
}

static inline long __wait_for_common(struct completion *x, long timeout)
{
	if (!x->done) {
		do {
		} while (!x->done && timeout);
		if (!x->done)
			return timeout;
	}
	x->done--;
	return timeout ?: 1;
}
#define wait_for_common(x, t, s)	__wait_for_common(x, t)
#define wait_for_common_io(x, t, s)	__wait_for_common(x, t)

static inline void wait_for_completion(struct completion *x)
{
	wait_for_common(x, MAX_SCHEDULE_TIMEOUT, TASK_UNINTERRUPTIBLE);
}
static inline void wait_for_completion_io(struct completion *x)
{
	wait_for_common_io(x, MAX_SCHEDULE_TIMEOUT, TASK_UNINTERRUPTIBLE);
}
static inline int wait_for_completion_interruptible(struct completion *x)
{
#if 1
	wait_for_common(x, MAX_SCHEDULE_TIMEOUT, TASK_INTERRUPTIBLE);
#else
	long t = wait_for_common(x, MAX_SCHEDULE_TIMEOUT, TASK_INTERRUPTIBLE);
	if (t == -ERESTARTSYS)
		return t;
#endif
	return 0;
}
static inline int wait_for_completion_killable(struct completion *x)
{
#if 1
	wait_for_common(x, MAX_SCHEDULE_TIMEOUT, TASK_KILLABLE);
#else
	long t = wait_for_common(x, MAX_SCHEDULE_TIMEOUT, TASK_KILLABLE);
	if (t == -ERESTARTSYS)
		return t;
#endif
	return 0;
}
static inline unsigned long
wait_for_completion_timeout(struct completion *x, unsigned long timeout)
{
	return wait_for_common(x, timeout, TASK_UNINTERRUPTIBLE);
}
static inline unsigned long
wait_for_completion_io_timeout(struct completion *x, unsigned long timeout)
{
	return wait_for_common_io(x, timeout, TASK_UNINTERRUPTIBLE);
}
static inline long
wait_for_completion_interruptible_timeout(struct completion *x,
					  unsigned long timeout)
{
	return wait_for_common(x, timeout, TASK_INTERRUPTIBLE);
}
static inline long
wait_for_completion_killable_timeout(struct completion *x,
				     unsigned long timeout)
{
	return wait_for_common(x, timeout, TASK_KILLABLE);
}
static inline bool try_wait_for_completion(struct completion *x)
{
	int ret = 1;
	if (!x->done)
		ret = 0;
	else
		x->done--;
	return ret;
}
static inline bool completion_done(struct completion *x)
{
	int ret = 1;
	if (!x->done)
		ret = 0;
	return ret;
}

static inline void complete(struct completion *x)
{
	x->done++;
	__wake_up_locked(&x->wait, TASK_NORMAL, 1);
}
static inline void complete_all(struct completion *x)
{
	x->done += UINT_MAX/2;
	__wake_up_locked(&x->wait, TASK_NORMAL, 0);
}

#endif /* !LIBKLIB_COMPLETION_H */
