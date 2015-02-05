/*
 * A daemon to flush dirty data to disk taking consistency into account.
 *
 * Copyright (c) 2008-2014 OGAWA Hirofumi
 */

#ifdef TUX3_FLUSHER_SYNC
#include "tux3.h"

static void __tux3_init_flusher(struct sb *sb)
{
#ifdef __KERNEL__
	/* Disable writeback task to control inode reclaim by dirty flags */
	vfs_sb(sb)->s_bdi = &noop_backing_dev_info;
#endif
}

int tux3_init_flusher(struct sb *sb)
{
	__tux3_init_flusher(sb);
	return 0;
}

void tux3_exit_flusher(struct sb *sb)
{
}

static void schedule_flush_delta(struct sb *sb)
{
	/* Wake up waiters for pending delta staging */
	wake_up_all(&sb->delta_event_wq);
}

static int flush_pending_delta(struct sb *sb)
{
	int err = 0;

	if (!test_bit(TUX3_COMMIT_PENDING_BIT, &sb->backend_state))
		goto out;

	if (test_and_clear_bit(TUX3_COMMIT_PENDING_BIT, &sb->backend_state))
		err = flush_delta(sb, COMMIT_SYNC);
out:
	return err;
}

/* Try delta transition */
static void try_delta_transition(struct sb *sb)
{
	trace("stage %u, backend_state %lx",
	      sb->staging_delta, sb->backend_state);
	if (!test_and_set_bit(TUX3_COMMIT_RUNNING_BIT, &sb->backend_state))
		delta_transition(sb);
}

/* Do the delta transition until specified delta */
static int try_delta_transition_until_delta(struct sb *sb, unsigned delta)
{
	trace("delta %u, stage %u, backend_state %lx",
	      delta, sb->staging_delta, sb->backend_state);

	/* Already delta transition was started for delta */
	if (delta_after_eq(sb->staging_delta, delta))
		return 1;

	if (!test_and_set_bit(TUX3_COMMIT_RUNNING_BIT, &sb->backend_state)) {
		/* Recheck after grabed TUX3_COMMIT_RUNNING_BIT */
		if (delta_after_eq(sb->staging_delta, delta)) {
			clear_bit(TUX3_COMMIT_RUNNING_BIT, &sb->backend_state);
			return 1;
		}

		delta_transition(sb);
	}

	return delta_after_eq(sb->staging_delta, delta);
}

/* Advance delta transition until specified delta */
static int wait_for_transition(struct sb *sb, unsigned delta)
{
	return wait_event_killable(sb->delta_event_wq,
				   try_delta_transition_until_delta(sb, delta));
}

static int try_flush_pending_until_delta(struct sb *sb, unsigned delta)
{
	trace("delta %u, committed %u, backend_state %lx",
	      delta, sb->committed_delta, sb->backend_state);

	if (!delta_after_eq(sb->committed_delta, delta))
		flush_pending_delta(sb);

	return delta_after_eq(sb->committed_delta, delta);
}

static int wait_for_commit(struct sb *sb, unsigned delta)
{
	return wait_event_killable(sb->delta_event_wq,
				   try_flush_pending_until_delta(sb, delta));
}

static int sync_current_delta(struct sb *sb, enum unify_flags unify_flag)
{
	struct delta_ref *delta_ref;
	unsigned delta;
	int err = 0;

	down_write(&sb->delta_lock);
	/* Get delta that have to write */
	delta_ref = delta_get(sb);
#ifdef UNIFY_DEBUG
	delta_ref->unify_flag = unify_flag;
#endif
	delta = delta_ref->delta;
	delta_put(sb, delta_ref);

	trace("delta %u", delta);

	/* Make sure the delta transition was done for current delta */
	err = wait_for_transition(sb, delta);
	if (err)
		return err;
	assert(delta_after_eq(sb->staging_delta, delta));

	/* Wait until committing the current delta */
	err = wait_for_commit(sb, delta);
	assert(err || delta_after_eq(sb->committed_delta, delta));
	up_write(&sb->delta_lock);
	return err;
}

#else /* !TUX3_FLUSHER_SYNC */
static void try_delta_transition(struct sb *sb)
{
	/* do nothing */
}

static inline int can_transition(struct sb *sb, unsigned delta)
{
	unsigned next_frontend_delta = sb->delta_waitref + 2;

	/*
	 * - No in-flight delta on the wait ref.
	 * - There is a free delta or more.
	 * - Transition for "delta" is still not done.
	 *
	 * (sb->delta_waitref + 2 == next frontend delta)
	 */
	return sb->delta_waitref == sb->delta_pending &&
		delta_after_eq(sb->delta_free, next_frontend_delta) &&
		delta_before(sb->delta_waitref, delta);
}

/* Do the delta transition until specified delta */
static int transition_until_delta(struct sb *sb, unsigned delta)
{
	trace("delta %u, delta_waitref %u, backend_state %lx",
	      delta, sb->delta_waitref, sb->backend_state);

	if (can_transition(sb, delta) &&
	    !test_and_set_bit(TUX3_STATE_TRANSITION_BIT, &sb->backend_state)) {
		/* Recheck after grabbed TUX3_STATE_TRANSITION_BIT */
		if (can_transition(sb, delta))
			delta_transition(sb);

		clear_bit(TUX3_STATE_TRANSITION_BIT, &sb->backend_state);
	}

	return delta_after_eq(sb->delta_waitref, delta);
}

/* Wait refcount == 0 to get pending delta for next staging delta. */
static void tux3_wait_for_pending(struct sb *sb)
{
	unsigned delta = sb->delta_staging + 1;
	struct delta_ref *delta_ref = to_delta_ref(sb, delta);

	trace("delta %u", delta);
	/* Make sure transition was done for this delta. */
	wait_event(sb->delta_transition_wq, transition_until_delta(sb, delta));
	/* Make sure waitref was done for this delta. */
	wait_for_completion(&delta_ref->waitref_done);

	/* Update staging delta to this delta. */
	sb->delta_staging = delta;
}

#define WB_REASON_TUX3_PENDING	((enum wb_reason)WB_REASON_MAX + 1)

long tux3_writeback(struct super_block *super, struct bdi_writeback *wb,
		    struct wb_writeback_work *work)
{
	struct sb *sb = tux_sb(super);
	unsigned target_delta;
	int err, flags;

	/* If we didn't finish replay yet, don't flush. */
	if (!(super->s_flags & MS_ACTIVE))
		return 0;

	/*
	 * We ignore WB_REASON_SYNC for "sync".  Because we handle
	 * "sync" request by ->sync_fs() with WB_REASON_TUX3_PENDING.
	 */
	if (work->reason == WB_REASON_SYNC)
		goto out;

	if (work->reason == WB_REASON_TUX3_PENDING) {
		struct tux3_wb_work *wb_work;
		/* Specified target delta for staging. */
		wb_work = container_of(work, struct tux3_wb_work, work);
		target_delta = wb_work->delta;
	} else {
		/* One delta for staging. */
		target_delta = sb->delta_staging + 1;
	}

	/* target_delta may already be flushed. */
	if (delta_before(sb->delta_staging, target_delta)) {
		/* Make sure the pending delta is there. */
		tux3_wait_for_pending(sb);

		flags = (work->sync_mode == WB_SYNC_ALL ? COMMIT_SYNC : 0);
		err = flush_delta(sb, flags);
		/* FIXME: error handling */
	}
	assert(delta_after_eq(sb->delta_staging, target_delta));

out:
	/* FIXME: set proper nr_pages */
	work->nr_pages = 0;

	return 1;
}

static int tux3_wait_for_transition(struct sb *sb, unsigned *result_delta)
{
	struct delta_ref *delta_ref;
	unsigned delta;

	/*
	 * Get delta number that we have to sync, then wait until
	 * backend is ready to process next request. Instead of adding
	 * sync request to bdi flusher for each calls.
	 *
	 * With this way, if backend is busy for flushing, tasks is
	 * blocked until backend is ready to sync. So sync requests
	 * from tasks are merged into one request.
	 *
	 *    task1 ****|      ||
	 *    task2       ***| ||
	 *    task3    ***|    ||
	 *    task4           *||**|
	 *
	 *  backend -- busy-----#
	 *                  transition
	 *
	 * For example, 4 tasks make the dirty data and called sync(2).
	 * While it, the backend was busy, and couldn't transition. In
	 * this case, task1, task2, and task3 get same delta number,
	 * then merged into one commit (group commit).
	 *
	 * This can improve the situation on heavy load.
	 */
	delta_ref = delta_get(sb);
	delta = delta_ref->delta;
	delta_put(sb, delta_ref);

	*result_delta = delta;

	trace("delta %u", delta);
	return wait_event_killable(sb->delta_transition_wq,
				   transition_until_delta(sb, delta));
}

static int tux3_wait_for_commit(struct sb *sb, unsigned delta)
{
	trace("delta %u", delta);
	return wait_event_killable(sb->delta_commit_wq,
				   delta_after_eq(sb->delta_commit, delta));
}

static int sync_current_delta(struct sb *sb, enum unify_flags unify_flag)
{
	unsigned delta;
	int err;

	/* FORCE_UNIFY is not supported */
	WARN_ON(unify_flag == FORCE_UNIFY);

	err = tux3_wait_for_transition(sb, &delta);
	if (!err)
		err = tux3_wait_for_commit(sb, delta);
	return err;
}

static struct tux3_wb_work *tux3_alloc_wb_work(struct sb *sb, unsigned delta)
{
	return &sb->wb_work[delta % ARRAY_SIZE(sb->wb_work)];
}

/* Schedule to flush the pending delta. */
static void tux3_wb_queue_work(struct sb *sb, struct delta_ref *delta_ref)
{
	struct tux3_wb_work *wb_work = tux3_alloc_wb_work(sb, delta_ref->delta);

	/* Must be enable to reuse this */
	assert(wb_work->dummy_done.done == 1);

	wb_work->delta = delta_ref->delta;
	reinit_completion(&wb_work->dummy_done);

	wb_work->work = (struct wb_writeback_work){
		.nr_pages	= LONG_MAX,
		.sync_mode	= WB_SYNC_ALL,
		.reason		= WB_REASON_TUX3_PENDING,
		/* This is just to avoid that bdi flusher kfree this. */
		.done		= &wb_work->dummy_done,
	};
	writeback_queue_work_sb(vfs_sb(sb), &wb_work->work);
}

static void schedule_flush_delta(struct sb *sb, struct delta_ref *delta_ref)
{
	int flusher_is_waiting;

	trace("delta waitref %u", delta_ref->delta);

	/* Wake up if flusher is already waiting refcount. */
	flusher_is_waiting = waitqueue_active(&delta_ref->waitref_done.wait);
	complete(&delta_ref->waitref_done);

	/* If flusher is already waiting for this delta, don't need to tell. */
	if (!flusher_is_waiting)
		tux3_wb_queue_work(sb, delta_ref);

	/* Allow to start new transition */
	sb->delta_pending++;
	wake_up_all(&sb->delta_transition_wq);
}
#endif /* !TUX3_FLUSHER_SYNC */
