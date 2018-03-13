/*
 * A daemon to flush dirty data to disk taking consistency into account.
 *
 * Copyright (c) 2008-2014 OGAWA Hirofumi
 */

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

#ifdef TUX3_FLUSHER_SYNC
static void tux3_init_flusher(struct sb *sb)
{
#ifdef __KERNEL__
	/* Disable writeback task to control inode reclaim by dirty flags */
	vfs_sb(sb)->s_bdi = &noop_backing_dev_info;
#endif
}

static int need_delta(struct sb *sb)
{
	static unsigned crudehack;
	return !(++crudehack % 10);
}

static int flush_latest_delta(struct sb *sb, int flags)
{
	trace("waitref %u", sb->delta_waitref);
	delta_transition(sb);

	/* Make sure the pending delta is there. */
	tux3_wait_for_pending(sb);
	assert(sb->delta_waitref == sb->delta_pending);
	assert(sb->delta_pending == sb->delta_staging);

	trace("staging %u", sb->delta_staging);
	return flush_delta(sb, flags);
}

/* Try flush delta */
static int try_flush_delta(struct sb *sb)
{
	int err = 0;

	down_write(&sb->delta_lock);
	if (need_delta(sb))
		err = flush_latest_delta(sb, FLUSH_NORMAL);
	up_write(&sb->delta_lock);

	return err;
}

static int __sync_current_delta(struct sb *sb, int flags)
{
	struct delta_ref *delta_ref;
	unsigned delta;
	int err = 0;

	down_write(&sb->delta_lock);

	/* Get delta that have to write */
	delta_ref = delta_get(sb);
	delta = delta_ref->delta;
	delta_put(sb, delta_ref);

	err = flush_latest_delta(sb, flags);
	assert(err || delta_after_eq(sb->delta_commit, delta));

	up_write(&sb->delta_lock);

	return err;
}

static void tux3_queue_wb_work(struct sb *sb, struct delta_ref *delta_ref)
{
	/* TUX3_FLUSHER_SYNC, so nothing to do */
}
#else /* !TUX3_FLUSHER_SYNC */
/*
 * BDI and tux3_wb_work interaction.
 *
 * When delta refcount == 0, schedule_flush_delta() queues tux3_wb_work
 * if needed. But BDI can be run by non-tux3 requests (e.g. WB_REASON_PERIODIC).
 * So if WB_REASON (current dequeued wb_work) is not WB_REASON_TUX3_PENDING,
 * we have to dequeue tux3_wb_work for current delta. (otherwise, we need
 * unsure number of tux3_wb_work[].)
 *
 * To make sure dequeue, we wait queuing of tux3_wb_work by
 * tux3_wait_for_pending(), then dequeue it. (Special optimize is
 * ->flusher_is_waiting. If schedule_flush_delta() sees
 * ->flusher_is_waiting==1, schedule_flush_delta() skips to queue
 * tux3_wb_work. We check "wb_work->work.list" if it was skipped.)
 */

#define WB_REASON_TUX3_PENDING	((enum wb_reason)WB_REASON_MAX + 1)

static struct tux3_wb_work *tux3_to_wb_work(struct sb *sb, unsigned delta)
{
	return &sb->wb_work[delta % ARRAY_SIZE(sb->wb_work)];
}

/*
 * Mark as flusher is waiting this delta already.
 *
 * See above comment "BDI and tux3_wb_work interaction."
 */
static void tux3_start_wb_work(struct sb *sb)
{
	unsigned delta = sb->delta_staging + 1;
	struct tux3_wb_work *wb_work = tux3_to_wb_work(sb, delta);
	wb_work->flusher_is_waiting = 1;
}

/*
 * When this is called, we know the flusher is working for target
 * delta. So make sure the tux3_wb_work (needless anymore) that queued
 * by tux3_queue_wb_work() is removed from bdi queue.
 *
 * See above comment "BDI and tux3_wb_work interaction."
 */
static void tux3_dequeue_wb_work(struct sb *sb, struct bdi_writeback *wb)
{
	struct tux3_wb_work *wb_work = tux3_to_wb_work(sb, sb->delta_staging);

	if (!list_empty(&wb_work->work.list)) {
		spin_lock_bh(&wb->work_lock);
		list_del_init(&wb_work->work.list);
		spin_unlock_bh(&wb->work_lock);
	}

	wb_work->flusher_is_waiting = 0;
}

/*
 * Schedule to flush the pending delta, to make sure the flusher
 * starts to work for this delta (the flusher may already be working
 * on this delta for other reason. E.g. periodical flush).
 *
 * See above comment "BDI and tux3_wb_work interaction."
 */
static void tux3_queue_wb_work(struct sb *sb, struct delta_ref *delta_ref)
{
	struct tux3_wb_work *wb_work = tux3_to_wb_work(sb, delta_ref->delta);

	/* Must be empty to reuse this */
	assert(list_empty(&wb_work->work.list));

	/* If flusher is already waiting for this delta, don't need to tell. */
	if (wb_work->flusher_is_waiting)
		return;

	wb_work->delta = delta_ref->delta;

	wb_work->work = (struct wb_writeback_work){
		.nr_pages	= LONG_MAX,
		.sync_mode	= WB_SYNC_ALL,
		.reason		= WB_REASON_TUX3_PENDING,
	};
	writeback_queue_work_sb(vfs_sb(sb), &wb_work->work);
}

long tux3_writeback(struct super_block *super, struct bdi_writeback *wb,
		    struct wb_writeback_work *work)
{
	struct sb *sb = tux_sb(super);
	unsigned target_delta;
	int err;

	/* If we didn't finish replay yet, don't flush. */
	if (!(super->s_flags & SB_ACTIVE))
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
		int flags = (work->sync_mode == WB_SYNC_ALL)
			? FLUSH_SYNC : FLUSH_NORMAL;

		/* Tell flusher is starting to wait for this delta */
		if (work->reason != WB_REASON_TUX3_PENDING)
			tux3_start_wb_work(sb);

		/* Make sure the pending delta is there. */
		tux3_wait_for_pending(sb);

		/* Remove tux3_wb_work for this delta if need */
		if (work->reason != WB_REASON_TUX3_PENDING)
			tux3_dequeue_wb_work(sb, wb);

		err = flush_delta(sb, flags);
		/* FIXME: error handling */
	}
	assert(delta_after_eq(sb->delta_staging, target_delta));

out:
	/* FIXME: set proper nr_pages */
	work->nr_pages = 0;

	return 1;
}

/* If umount path, doesn't allow to kill. */
#define tux3_wait_event_may_killable(um, wq, cond)	({	\
	int _ret;						\
	if (um) {						\
		wait_event(wq, cond);				\
		_ret = 0;					\
	} else							\
		_ret = wait_event_killable(wq, cond);		\
	_ret;							\
})

static int tux3_wait_for_transition(struct sb *sb, unsigned *result_delta,
				    int is_umount)
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
	return tux3_wait_event_may_killable(is_umount, sb->delta_transition_wq,
					    transition_until_delta(sb, delta));
}

static int tux3_wait_for_commit(struct sb *sb, unsigned delta, int is_umount)
{
	trace("delta %u", delta);
	return tux3_wait_event_may_killable(is_umount, sb->delta_commit_wq,
				delta_after_eq(sb->delta_commit, delta));
}

static int __sync_current_delta(struct sb *sb, int flags)
{
	unsigned delta;
	int err, is_umount = (vfs_sb(sb)->s_root == NULL);

	/*
	 * FORCE_DELTA and FORCE_UNIFY are not supported. And there is
	 * no way to pass "flags" to tux3_writeback() reliably.
	 */
	WARN_ON(flags & ~(FLUSH_NORMAL | FLUSH_SYNC));

	err = tux3_wait_for_transition(sb, &delta, is_umount);
	if (!err)
		err = tux3_wait_for_commit(sb, delta, is_umount);

	if (is_umount) {
		/*
		 * On umount path: Wait for finishing backend completely,
		 * and drain work items in the queue of flusher.
		 *
		 * NOTE: we ignore WB_REASON_SYNC, so no extra commit by this.
		 */
		sync_inodes_sb(vfs_sb(sb));
	}

	return err;
}
#endif /* !TUX3_FLUSHER_SYNC */

/* Synchronous flush (without unify if possible). */
int sync_current_delta(struct sb *sb)
{
	return __sync_current_delta(sb, FLUSH_SYNC);
}

static void schedule_flush_delta(struct sb *sb, struct delta_ref *delta_ref)
{
	trace("delta waitref %u", delta_ref->delta);
	/* Add request for this delta to flusher. */
	tux3_queue_wb_work(sb, delta_ref);

	/* Complete after queued tux3_wb_work */
	complete(&delta_ref->waitref_done);

	/* Allow to start new transition */
	sb->delta_pending++;
	wake_up_all(&sb->delta_transition_wq);
}
