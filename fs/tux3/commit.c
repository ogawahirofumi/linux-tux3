/*
 * Commit a filesystem delta atomically to media.
 *
 * Delta pipeline stages/states:
 *
 *        transition                     backend
 *           |.|                       |.........|
 *   frontend +=> wait ref => pending => running => free
 *   |        |                                        |
 *   +-<- [free => next frontend] <--------------------+
 *
 * Each stage can have only one delta at once. If new delta is going
 * to enter busy stage, we have to wait the next stage become free.
 *
 * frontend:   Frontend adds dirty data to delta.
 *
 * transition: This is not the stage though. Related operation. Switch
 *             the delta for adding dirty data to new delta.  After
 *             this, frontends can't grab the reference to old delta
 *             anymore.
 *
 * wait ref:   Frontend can't grab this delta, but some frontends may
 *             still referencing. All frontends released the reference
 *             to this delta, this become pending state.
 *
 * pending:    Reference == 0, so ready to start backend.
 *
 * running:    Backend is writing this delta to backing storage.
 *
 * free:       Backend was done to commit, and can reuse for frontend.
 *
 * Copyright (c) 2008-2015 Daniel Phillips
 * Copyright (c) 2008-2015 OGAWA Hirofumi
 */

#include "tux3.h"
#include "ioinfo.h"
#ifdef __KERNEL__
#include <linux/kthread.h>
#include <linux/freezer.h>
#endif

#ifndef trace
#define trace trace_off
#endif

static void tux3_wake_delta_commit(struct sb *sb);
static void tux3_wake_delta_free(struct sb *sb);
static void schedule_flush_delta(struct sb *sb, struct delta_ref *delta_ref);

/*
 * Need frontend modification of backend buffers. (modification
 * after latest delta commit and before unify).
 *
 * E.g. frontend modified backend buffers, stage_delta() of when
 * unify is called.
 */
#define ALLOW_FRONTEND_MODIFY


/*
 * Deferred free blocks list
 */

void defer_bfree_init(struct defree *defree)
{
	stash_init(&defree->stash);
	defree->blocks = 0;
}

void destroy_defer_bfree(struct defree *defree)
{
	empty_stash(&defree->stash);
	defree->blocks = 0;
}

int defer_bfree(struct sb *sb, struct defree *defree, block_t block,
		unsigned count)
{
	static const unsigned limit = ULLONG_MAX >> 48;

	assert(count > 0);
	assert(block + count <= sb->volblocks);

	/*
	 * count field of stash is 16bits. So, this separates to
	 * multiple records to avoid overflow.
	 */
	while (count) {
		unsigned c = min(count, limit);
		int err;

		err = stash_value(&defree->stash, ((u64)c << 48) + block);
		if (err)
			return err;

		defree->blocks += c;
		count -= c;
		block += c;
	}

	return 0;
}

#ifdef ALLOW_FRONTEND_MODIFY
/* Re-log defree by frontend on this cycle. */
static int relog_frontend_defree_fn(u64 val, void *data)
{
	struct sb *sb = data;
	log_bfree_relog(sb, val & ((1ULL << 48) - 1), val >> 48);
	return 0;
}

static void relog_frontend_defree(struct sb *sb)
{
	stash_walk(&sb->defree.stash, relog_frontend_defree_fn, sb);
}
#endif

/* Re-log and move deunify to defree for obsoleting previous unify. */
static int move_deunify_to_defree_fn(u64 val, void *data)
{
	struct sb *sb = data;
	int err = stash_value(&sb->defree.stash, val);
	if (!err) {
		block_t block = val & ((1ULL << 48) - 1);
		unsigned count = val >> 48;
		log_bfree_relog(sb, block, count);

		sb->defree.blocks += count;
		sb->deunify.blocks -= count;
	}
	return err;
}

static int move_deunify_to_defree(struct sb *sb)
{
	return unstash(&sb->deunify.stash, move_deunify_to_defree_fn, sb);
}

/* Apply defree to bitmap. */
static int defree_apply_bfree_fn(u64 val, void *data)
{
	struct sb *sb = data;
	block_t block = val & ((1ULL << 48) - 1);
	unsigned count = val >> 48;
	int err = bfree(sb, block, count);
	if (!err)
		sb->defree.blocks -= count;
	return err;
}

static int defree_apply_bfree(struct sb *sb)
{
	return unstash(&sb->defree.stash, defree_apply_bfree_fn, sb);
}

/*
 * ENOSPC management
 */

/* FIXME: magic number */
enum {
	min_reserve = 32,
	max_reserve = 128,
};

block_t nospc_min_reserve(void)
{
	return min_reserve;
}

/* Cost for in-flight orphans. */
static inline block_t nospc_orphan_cost(struct sb *sb)
{
	return sb->orphan.count * TUX3_COST_ORPHAN +
		sb->orphan.count_del * TUX3_COST_ORPHAN_DEL;
}

/* Available free blocks for next delta */
static block_t nospc_free(struct sb *sb)
{
	/* orphan_cost is used as non-free blocks */
	return sb->freeblocks + sb->defree.blocks - nospc_orphan_cost(sb);
}

/*
 * Reserve size should vary with budget. The reserve can include the
 * log block overhead on the assumption that every block in the budget
 * is a data block that generates one log record (or two?).
 */
static inline block_t nospc_reserve(block_t free)
{
	/* FIXME: magic number */
	return clamp_val(free >> 7, min_reserve, max_reserve);
}

/*
 * After transition, the front delta may have used some of the balance
 * left over from this delta. The charged amount of the back delta is
 * now stable and gives the exact balance at transition by subtracting
 * from the old budget. The difference between the new budget and the
 * balance at transition, which must never be negative, is added to
 * the current balance, so the effect is exactly the same as if we had
 * set the new budget and balance atomically at transition time. But
 * we do not know the new balance at transition time and even if we
 * did, we would need to add serialization against frontend changes,
 * which are currently lockless and would like to stay that way. So we
 * let the current delta charge against the remaining balance until
 * flush is done, here, then adjust the balance to what it would have
 * been if the budget had been reset exactly at transition.
 *
 * We have:
 *
 *    consumed = oldfree - free
 *    oldbudget = oldfree - reserve
 *    newbudget = free - reserve
 *    transition_balance = oldbudget - charged
 *
 * Factoring out the reserve, the balance adjustment is:
 *
 *    adjust = newbudget - transition_balance
 *           = (free - reserve) - ((oldfree - reserve) - charged)
 *           = free + (charged - oldfree)
 *           = charged + (free - oldfree)
 *           = charged - consumed
 *
 * To extend for variable reserve size, add the difference between
 * old and new reserve size to the balance adjustment.
 *
 *    adjust = oldreserve - reserve
 *
 * We have to keep cost for orphan until finishing real deletion of
 * inode. To keep cost, we track number of orphans, and calculate
 * orphan_cost from it. Then, we think the orphan_cost as non-free
 * blocks.  (because orphan is pinned by user control, so have to
 * return ENOSPC, instead of waiting).
 *
 *    free -= orphan_cost
 */
static void nospc_adjust_balance(struct sb *sb, unsigned delta,
				 block_t unify_cost)
{
	enum { initial_logblock = 0 };
	struct nospc_data *nospc = &sb->nospc;
	block_t charged = atomic64_read(&tux3_sb_ddc(sb, delta)->nospc_charged);
	block_t old_free = nospc->free;
	block_t old_reserve = nospc->reserve;
	block_t consumed, adjust_reserve;

	/* Re-initialize charged for future. */
	atomic64_set(&tux3_sb_ddc(sb, delta)->nospc_charged, 0);

	/* Update budget for next delta. */
	nospc->free = nospc_free(sb);
	nospc->reserve = nospc_reserve(nospc->free);
	atomic64_set(&nospc->budget, nospc->free - nospc->reserve);

	/*
	 * Adjust balance to new state. (Note, both can be negative)
	 *
	 * 1) Add over estimated blocks (charged - consumed).
	 * 2) Add adjusted reserve (old_reserve - reserve).
	 */
	consumed = old_free - nospc->free;
	adjust_reserve = old_reserve - nospc->reserve;
	atomic64_add((charged - consumed) + adjust_reserve, &nospc->balance);

	trace("budget %Ld, balance %Ld, charged %Lu, consumed %Ld, freeblocks %Lu, defree %Lu, unify %Lu",
	      (long long)atomic64_read(&nospc->budget),
	      (long long)atomic64_read(&nospc->balance),
	      charged, consumed, sb->freeblocks, sb->defree.blocks, unify_cost);

	WARN(consumed - initial_logblock - unify_cost > charged,
	     "delta %u estimate exceeded by %Lu blocks\n",
	     delta, consumed - charged);
}

/*
 * nospc_atomic64_add_if_ge - add if the number is greater equal than
 * a given value
 * @v: pointer of type atomic64_t
 * @a: the amount to add to v...
 * @u: ...unless v is greater equal than u.
 *
 * Atomically adds @a to @v, so long as it was greater than @u.
 * Returns true if success to sub.
 *
 * FIXME: move to core.
 */
static inline long long
nospc_atomic64_add_if_ge(atomic64_t *v, long long a, long long u)
{
	long c, old;
	c = atomic64_read(v);
	for (;;) {
		if (unlikely(c < u))
			break;
		old = atomic64_cmpxchg(v, c, c + a);
		if (likely(old == c))
			break;
		c = old;
	}
	return c >= u;
}

/*
 * Add cost to balance if balance is greater than limit.
 * limit: negative limit is to use reserve blocks.
 *
 * FIXME: If file is temporary, we may want to remove cost from
 * balance when unlinked. But is there a value to add field to
 * remember per-inode cost?
 */
static inline int nospc_add_cost(struct sb *sb, int cost, int limit)
{
	struct nospc_data *nospc = &sb->nospc;

	/* Subs "cost" from "balance" if result is >= "limit" */
	if (nospc_atomic64_add_if_ge(&nospc->balance, -cost, limit + cost)) {
		unsigned delta = tux3_get_current_delta();
		atomic64_add(cost, &tux3_sb_ddc(sb, delta)->nospc_charged);
		return 0;
	}
	return -ENOSPC;
}

int nospc_wait_and_check(struct sb *sb, int cost, int limit)
{
	struct nospc_data *nospc = &sb->nospc;

	/*
	 * FIXME: We want to wait update of budget/balance, not full commit.
	 * FIXME: Retry can be unfair, so we may want to use queue to wakeup?
	 * I.e. one process may stay in loop of ENOSPC check so long.
	 */
	assert(!change_active());
	sync_current_delta(sb);

	trace("test budget, budget %Ld, balance %Ld, cost %u, limit %d",
	      (long long)atomic64_read(&nospc->budget),
	      (long long)atomic64_read(&nospc->balance),
	      cost, limit);

	if (limit + cost > atomic64_read(&nospc->budget)) {
		tux3_msg(sb, "*** out of space ***");
		return -ENOSPC;
	}

	return 0;
}

void nospc_init_balance(struct sb *sb)
{
	struct nospc_data *nospc = &sb->nospc;

	nospc->free = 0;
	nospc->reserve = 0;
	atomic64_set(&nospc->budget, 0);
	atomic64_set(&nospc->balance, 0);

	/* Set initial budget/balance */
	nospc_adjust_balance(sb, TUX3_INIT_DELTA, 0);
}

/* Estimate backend allocation cost per data page */
unsigned nospc_one_page_cost(struct inode *inode)
{
	struct sb *sb = tux_sb(inode->i_sb);
	struct btree *btree = &tux_inode(inode)->btree;
	unsigned depth = has_root(btree) ? btree->root.depth : 0;
	return sb->blocks_per_page + 2 * depth + 1;
}

/*
 * Commit stuff
 */

/* FIXME: we are using sb for now, should use metablock instead. */
static int save_metablock(struct sb *sb, unsigned int req_flags)
{
	struct disksuper *super = &sb->super;

	super->blockbits = cpu_to_be16(sb->blockbits);
	super->volblocks = cpu_to_be64(sb->volblocks);

	/* Probably does not belong here (maybe metablock) */
	super->iroot = cpu_to_be64(pack_root(&itree_btree(sb)->root));
	super->oroot = cpu_to_be64(pack_root(&otree_btree(sb)->root));
	super->nextblock = cpu_to_be64(sb->nextblock);
	super->atomdictsize = cpu_to_be64(sb->atomdictsize);
	super->freeatom = cpu_to_be32(sb->freeatom);
	super->atomgen = cpu_to_be32(sb->atomgen);
	/* logchain and logcount are written to super directly */

	return devio_sync(REQ_OP_WRITE, REQ_META | req_flags,
			  sb_dev(sb), SB_LOC, super, SB_LEN);
}

/* Delta transition */

/* Obsolete the old unify, then start the log of new unify */
static void new_cycle_log(struct sb *sb)
{
#if 0 /* ALLOW_FRONTEND_MODIFY */
	/*
	 * FIXME: we don't need to write the logs generated by
	 * frontend at all.  However, for now, we are writing those
	 * logs for debugging.
	 */

	/* Discard the logs generated by frontend. */
	log_finish_cycle(sb, 1);
#endif
	/* Initialize logcount to count log blocks on new unify cycle. */
	sb->super.logcount = 0;
}

/*
 * Flush a snapshot of the allocation map to disk.  Physical blocks for
 * the bitmaps and new or redirected bitmap btree nodes may be allocated
 * during the unify.  Any bitmap blocks that are (re)dirtied by these
 * allocations will be written out in the next unify cycle.
 */
static int unify_log(struct sb *sb)
{
	/* further block allocations belong to the next cycle */
	unsigned unify = sb->unify++;
	LIST_HEAD(orphan_add);
	LIST_HEAD(orphan_del);

	trace(">>>>>>>>> commit unify %u", unify);

	/*
	 * Orphan inodes are still living, or orphan inodes in
	 * sb->otree are dead. And logs will be obsoleted, so, we
	 * apply those to sb->otree.
	 */
	/* FIXME: orphan_add/del has no race with frontend for now */
	list_splice_init(&sb->orphan.add_head, &orphan_add);
	list_splice_init(&sb->orphan.del_head, &orphan_del);

	/* This is starting the new unify cycle of the log */
	new_cycle_log(sb);
	/* Add unify log as mark of new unify cycle. */
	log_unify(sb);
	/* Log to store freeblocks for flushing bitmap data */
	log_freeblocks(sb, sb->freeblocks);
#ifdef ALLOW_FRONTEND_MODIFY
	/*
	 * If frontend made defered bfree (i.e. it is not applied to
	 * bitmap yet), we have to re-log it on this cycle. Because we
	 * obsolete all logs in past.
	 */
	relog_frontend_defree(sb);
#endif
	/*
	 * Re-logging defered bfree blocks after unify as defered
	 * bfree (LOG_BFREE_RELOG) after delta.  With this, we can
	 * obsolete log records on previous unify.
	 */
	move_deunify_to_defree(sb);

	/*
	 * Merge the dirty bnode buffers to volmap dirty list, and
	 * clean ->unify_buffers up before dirtying bnode buffers on
	 * this unify.  Later, bnode blocks will be flushed via
	 * volmap with leaves.
	 */
	list_splice_init(&sb->unify_buffers,
			 tux3_dirty_buffers(sb->volmap, TUX3_INIT_DELTA));
	/*
	 * tux3_mark_buffer_unify() doesn't dirty inode, so we make
	 * sure volmap is dirty for unify buffers, now.
	 *
	 * See comment in tux3_mark_buffer_unify().
	 */
	__tux3_mark_inode_dirty(sb->volmap, I_DIRTY_PAGES);

	/* Flush bitmap */
	trace("> flush bitmap %u", unify);
	tux3_flush_inode_internal(sb->bitmap, unify, REQ_META);
	trace("< done bitmap %u", unify);

	/* Flush bitmap */
	trace("> flush countmap %u", unify);
	tux3_flush_inode_internal(sb->countmap, unify, REQ_META);
	trace("< done countmap %u", unify);

	trace("> apply orphan inodes %u", unify);
	{
		int err;

		/*
		 * This defered deletion of orphan from sb->otree.
		 * It should be done before adding new orphan, because
		 * orphan_add may have same inum in orphan_del.
		 */
		err = tux3_unify_orphan_del(sb, &orphan_del);
		if (err)
			return err;

		/*
		 * This apply orphan inodes to sb->otree after flushed bitmap.
		 */
		err = tux3_unify_orphan_add(sb, &orphan_add);
		if (err)
			return err;
	}
	trace("< apply orphan inodes %u", unify);
	assert(list_empty(&orphan_add));
	assert(list_empty(&orphan_del));
	trace("<<<<<<<<< commit unify done %u", unify);

	return 0;
}

/* Apply frontend modifications to backend buffers, and flush data buffers. */
static int stage_delta(struct sb *sb, unsigned delta)
{
	int err;

	/* Flush inodes */
	err = tux3_flush_inodes(sb, delta);
	if (err)
		return err;

	/* Flush atable after inodes. Because inode deletion may dirty atable */
	err = tux3_flush_inode_internal(sb->atable, delta, 0);
	if (err)
		return err;
#if 0
	/* FIXME: we have to flush vtable somewhere */
	err = tux3_flush_inode_internal(sb->vtable, delta, 0);
	if (err)
		return err;
#endif
	return err;
}

static int write_btree(struct sb *sb, unsigned delta)
{
	/*
	 * If page is still dirtied by unify buffer,
	 * tux3_mark_buffer_atomic() doesn't dirty inode for delta, so
	 * we make sure volmap is dirty for delta buffers here.
	 *
	 * FIXME: better way to do?
	 */
	__tux3_mark_inode_dirty(sb->volmap, I_DIRTY_PAGES);

	/*
	 * Flush leaves (and if there is unify, bnodes too) blocks.
	 * FIXME: Now we are using TUX3_INIT_DELTA for leaves. Do
	 * we need to per delta dirty buffers?
	 */
	return tux3_flush_inode_internal(sb->volmap, TUX3_INIT_DELTA, REQ_META);
}

/* allocate and write log blocks */
static int write_log(struct sb *sb)
{
	/* Finish to logging in this delta */
	log_finish_cycle(sb, 0);

	return tux3_flush_inode_internal(sb->logmap, TUX3_INIT_DELTA, REQ_META);
}

static int commit_delta(struct sb *sb)
{
	unsigned int req_flags = tux3_io_req_flags(sb->ioinfo);
	int err, barrier = TUX3_TEST_MOPT(sb, BARRIER);

	/* Wait I/O was submitted */
	tux3_io_wait(sb->ioinfo);

	/*
	 * FIXME: we don't need REQ_FUA here actually. But deferred
	 * free must be done after commit block was hit to media.
	 *
	 * We can optimize by delaying deferred free until after next
	 * REQ_PREFLUSH in next delta. Therefore, if make it async, we
	 * can start next delta more early.
	 *
	 * (But if data integrity path (sync, fsync, umount, etc.), we
	 * have to make sure last commit was done. So if those paths,
	 * we would need REQ_FUA here (or __sync_current_delta() such
	 * issues REQ_PREFLUSH instead?).
	 */
	if (barrier) {
		/*
		 * Don't add REQ_SYNC explicitly here to use the same
		 * CFQ-queue with previous, and to avoid CFQ's
		 * idle_slice_timer between CFQ-queues.
		 */
		req_flags |= REQ_PREFLUSH | REQ_FUA;
	}

	trace("commit %i logblocks", be32_to_cpu(sb->super.logcount));
	err = save_metablock(sb, req_flags);
	if (err)
		return err;

	tux3_wake_delta_commit(sb);

	/* Commit was finished, apply defered bfree. */
	return defree_apply_bfree(sb);
}

static void post_commit(struct sb *sb, unsigned delta)
{
	/*
	 * Check referencer of forked buffer was gone, and can free.
	 * FIXME: is this right timing and place to do this?
	 */
	free_forked_buffers(sb, NULL, 0);

	tux3_volmap_clean_io(sb->volmap);
	tux3_clear_dirty_inodes(sb, delta);
}

static int need_unify(struct sb *sb)
{
	static unsigned crudehack;
	return !(++crudehack % 3);
}

/* For debugging */
void tux3_start_backend(struct sb *sb)
{
	assert(!change_active());
	current->journal_info = sb;
}

void tux3_end_backend(void)
{
	assert(change_active());
	current->journal_info = NULL;
}

/* If true, it is under backend */
int tux3_under_backend(struct sb *sb)
{
	return current->journal_info == sb;
}

static int do_commit(struct sb *sb, int flags)
{
	unsigned delta = sb->delta_staging;
	int no_unify = flags & __NO_UNIFY;
	struct blk_plug plug;
	struct ioinfo ioinfo;
	block_t unify_cost = 0;
	int err = 0;

	trace(">>>>>>>>> commit delta %u", delta);
	/* further changes of frontend belong to the next delta */
	tux3_start_backend(sb);

	/*
	 * While umount, do_commit can race with umount. So, this
	 * checks if need to commit or not. Otherwise, e.g. sb->logmap
	 * can be freed already by ->put_super().
	 *
	 * And when sync/fsync() is called, we may not have dirty inodes.
	 *
	 * FIXME: there is no need to commit if normal inodes are not
	 * dirty? better way?
	 */
	if (!(flags & __FORCE_DELTA) && !tux3_has_dirty_inodes(sb, delta)) {
		/* There was no dirty, reset charged cost */
		nospc_adjust_balance(sb, delta, 0);
		goto out;
	}

	/* Prepare to wait I/O */
	tux3_io_init(&ioinfo, flags);
	sb->ioinfo = &ioinfo;

	/*
	 * Start plugging to merge early I/O dleaf with data, and
	 * possibly data with itree metadata.
	 */
	blk_start_plug(&plug);

	/* Add delta log for debugging. */
	log_delta(sb);

	/*
	 * NOTE: This works like modification from frontend. (i.e. this
	 * may generate defree log which is not committed yet at unify.)
	 *
	 * - this is before unify to merge modifications to this
	 *   unify, and flush at once for optimization.
	 *
	 * - this is required to prevent unexpected buffer state for
	 *   cursor_redirect(). If we applied modification after
	 *   unify_log, it made unexpected dirty state (i.e. leaf is
	 *   still dirty, but parent was already cleaned.)
	 */
	err = stage_delta(sb, delta);
	if (err)
		goto error; /* FIXME: error handling */

#if 0
	/*
	 * FIXME: If Synchronous flush, we would want to avoid unify
	 * for better latency. But maybe, we should check the some
	 * conditions of memory and freeblocks.
	 */
	if (flags & FLUSH_SYNC)
		no_unify = 1;
#endif

	if ((!no_unify && need_unify(sb)) || (flags & __FORCE_UNIFY)) {
		unify_cost = sb->freeblocks;
		err = unify_log(sb);
		if (err)
			goto error; /* FIXME: error handling */
		unify_cost -= sb->freeblocks;

		/* Add delta log for debugging. */
		log_delta(sb);
	}

	write_btree(sb, delta);
	write_log(sb);
	blk_finish_plug(&plug);

	/* Adjust ENOSPC state after all block allocation was done. */
	nospc_adjust_balance(sb, delta, unify_cost);

	/*
	 * Commit last block (for now, this is sync I/O).
	 *
	 * FIXME: If this is not data integrity write, we don't have
	 * to wait the commit block. The commit block just have to
	 * written before next block block. (But defree must be after
	 * commit block.)
	 */
	err = commit_delta(sb); /* FIXME: err */
out:
	/* Set to NULL for debugging */
	sb->ioinfo = NULL;
	/* FIXME: what to do if error? */
	tux3_end_backend();
	trace("<<<<<<<<< commit done %u: err %d", delta, err);

	post_commit(sb, delta);
	trace("<<<<<<<<< post commit done %u", delta);

	return err;

error:
	tux3_warn(sb, "commit error %d", err);
	blk_finish_plug(&plug);
	goto out;
}

/*
 * Flush delta work
 */

#define delta_after(a,b)			\
	(typecheck(unsigned, a) &&		\
	 typecheck(unsigned, b) &&		\
	 ((int)((b) - (a)) < 0))
#define delta_before(a,b)	delta_after(b,a)

#define delta_after_eq(a,b)			\
	(typecheck(unsigned, a) &&		\
	 typecheck(unsigned, b) &&		\
	 ((int)((a) - (b)) >= 0))
#define delta_before_eq(a,b)	delta_after_eq(b,a)

/* Internal use only */
static struct delta_ref *to_delta_ref(struct sb *sb, unsigned delta)
{
	return &sb->delta_refs[tux3_delta(delta)];
}

static int flush_delta(struct sb *sb, int flags)
{
	int err;

	err = do_commit(sb, flags);

	/*
	 * NOTE: We have to wakeup waiters even if error or skipped
	 * commit (no dirty inodes). So, check it.
	 */
	if (delta_before(sb->delta_commit, sb->delta_staging))
		tux3_wake_delta_commit(sb);

	tux3_wake_delta_free(sb);

	return err;
}

/*
 * Provide transaction boundary for delta, and delta transition request.
 */

static void tux3_wake_delta_commit(struct sb *sb)
{
	/* Wake up waiters for delta commit. */
	sb->delta_commit++;
	trace("delta_commit %u", sb->delta_commit);
	wake_up_all(&sb->delta_commit_wq);
}

static void tux3_wake_delta_free(struct sb *sb)
{
	/* Wake up waiters for free delta */
	sb->delta_free++;
	trace("delta_free %u", sb->delta_free);
	wake_up_all(&sb->delta_transition_wq);
}

/* Grab the reference of current delta */
static struct delta_ref *delta_get(struct sb *sb)
{
	struct delta_ref *delta_ref;
	/*
	 * Try to grab reference. If failed, retry.
	 *
	 * memory barrier pairs with __delta_transition(). But we never
	 * free ->current_delta, so we don't need rcu_read_lock().
	 */
	do {
		/*
		 * NOTE: Without this barrier(), at least, gcc-4.8.2 ignores
		 * volatile dereference of sb->current_delta in this loop,
		 * and instead using cached value.
		 * (Looks like gcc bug, and this barrier() is workaround of it)
		 */
		barrier();
		delta_ref = rcu_dereference_check(sb->current_delta, 1);
	} while (!refcount_inc_not_zero(&delta_ref->refcount));

	trace("delta %u, refcount %u",
	      delta_ref->delta, refcount_read(&delta_ref->refcount));

	return delta_ref;
}

/* Release the reference of delta */
static void delta_put(struct sb *sb, struct delta_ref *delta_ref)
{
	if (refcount_dec_and_test(&delta_ref->refcount))
		schedule_flush_delta(sb, delta_ref);

	trace("delta %u, refcount %u",
	      delta_ref->delta, refcount_read(&delta_ref->refcount));
}

/* Update current delta */
static void __delta_transition(struct sb *sb, struct delta_ref *delta_ref,
			       unsigned new_delta)
{
	assert(refcount_read(&delta_ref->refcount) == 0);
	/* Set the initial refcount. */
	refcount_set(&delta_ref->refcount, 1);
	/* Initialize waitref completion */
	reinit_completion(&delta_ref->waitref_done);
	/* Assign the delta number */
	delta_ref->delta = new_delta;

	/*
	 * Update current delta, then release reference.
	 *
	 * memory barrier pairs with delta_get().
	 */
	rcu_assign_pointer(sb->current_delta, delta_ref);
}

/*
 * Delta transition.
 *
 * Find the next delta_ref, then update current delta to it, and
 * release previous delta refcount.
 */
static void delta_transition(struct sb *sb)
{
	/*
	 * This is exclusive by TUX3_STATE_TRANSITION_BIT (no writer),
	 * so rcu_dereference may not be needed.
	 */
	struct delta_ref *prev = rcu_dereference_check(sb->current_delta, 1);
	struct delta_ref *delta_ref;

	/* Find the next delta_ref */
	delta_ref = to_delta_ref(sb, prev->delta + 1);

	/* Update the current delta. */
	__delta_transition(sb, delta_ref, prev->delta + 1);

	/* Update the waitref delta to delta that transition was done */
	sb->delta_waitref++;
	assert(sb->delta_waitref == prev->delta);

	/* Release initial refcount after updated the current delta. */
	delta_put(sb, prev);
	trace("prev %u, next %u", prev->delta, delta_ref->delta);
}

#include "commit_flusher.c"

void tux3_delta_init(struct sb *sb)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(sb->delta_refs); i++) {
		refcount_set(&sb->delta_refs[i].refcount, 0);
		init_completion(&sb->delta_refs[i].waitref_done);
	}
#ifdef TUX3_FLUSHER_SYNC
	init_rwsem(&sb->delta_lock);
#else
	for (i = 0; i < ARRAY_SIZE(sb->wb_work); i++) {
		INIT_LIST_HEAD(&sb->wb_work[i].work.list);
		sb->wb_work[i].flusher_is_waiting = 0;
	}
#endif
	init_waitqueue_head(&sb->delta_transition_wq);
	init_waitqueue_head(&sb->delta_commit_wq);
}

void tux3_delta_setup(struct sb *sb)
{
	sb->unify		= TUX3_INIT_DELTA;
	sb->delta_waitref	= TUX3_INIT_DELTA - 1;
	sb->delta_pending	= TUX3_INIT_DELTA - 1;
	sb->delta_staging	= TUX3_INIT_DELTA - 1;
	sb->delta_commit	= TUX3_INIT_DELTA - 1;
	sb->delta_free		= sb->delta_commit + TUX3_MAX_DELTA;

	/* Setup initial delta_ref */
	__delta_transition(sb, &sb->delta_refs[0], TUX3_INIT_DELTA);

#ifdef TUX3_FLUSHER_SYNC
	tux3_init_flusher(sb);
#endif
}

unsigned tux3_get_current_delta(void)
{
	struct delta_ref *delta_ref = current->journal_info;
	assert(delta_ref != NULL);
	return delta_ref->delta;
}

/* Choice sb->delta or sb->unify from inode */
unsigned tux3_inode_delta(struct inode *inode)
{
	if (tux3_inode_test_flag(TUX3_I_NO_DELTA, inode)) {
		/*
		 * Note: volmap are special, and has both of
		 * TUX3_INIT_DELTA and sb->unify. So TUX3_INIT_DELTA
		 * can be incorrect if delta was used for buffer.
		 * Note: logmap is similar to volmap, but it doesn't
		 * have sb->unify buffers.
		 */
		return TUX3_INIT_DELTA;
	}

	if (tux3_inode_test_flag(TUX3_I_UNIFY, inode))
		return tux_sb(inode->i_sb)->unify;

	return tux3_get_current_delta();
}

static inline void __change_begin_atomic(struct sb *sb)
{
	assert(!change_active());
	current->journal_info = delta_get(sb);
}

static inline void __change_end_atomic(struct sb *sb)
{
	struct delta_ref *delta_ref = current->journal_info;
	assert(change_active());
	current->journal_info = NULL;
	delta_put(sb, delta_ref);
}

/*
 * This is used to avoid to run backend (if disabled asynchronous
 * backend), and never be blocked. This is used in atomic context, or
 * from backend task to avoid to run backend recursively.
 */
void change_begin_atomic(struct sb *sb)
{
	__change_begin_atomic(sb);
}

/* change_end() without starting do_commit(). Use this only if necessary. */
void change_end_atomic(struct sb *sb)
{
	__change_end_atomic(sb);
}

/*
 * This is used for nested change_begin/end. We should not use this
 * usually (nesting change_begin/end is wrong for normal operations).
 *
 * For now, this is only used for ->evict_inode() debugging.
 */
void change_begin_atomic_nested(struct sb *sb, void **ptr)
{
	*ptr = current->journal_info;
	current->journal_info = NULL;
	__change_begin_atomic(sb);
}

void change_end_atomic_nested(struct sb *sb, void *ptr)
{
	__change_end_atomic(sb);
	current->journal_info = ptr;
}

/*
 * Normal version of change_begin/end. If there is no special
 * requirement, we should use this version.
 *
 * This checks backend job and run if disabled asynchronous backend.
 */

static inline void __change_begin(struct sb *sb)
{
#ifdef TUX3_FLUSHER_SYNC
	down_read(&sb->delta_lock);
#endif
	__change_begin_atomic(sb);
}

static inline void __change_end(struct sb *sb)
{
	__change_end_atomic(sb);
#ifdef TUX3_FLUSHER_SYNC
	up_read(&sb->delta_lock);
#endif
}

/* Start transaction without ENOSPC check. */
void change_begin_nocheck(struct sb *sb)
{
	__change_begin(sb);
}

/* Start transaction with first ENOSPC check. */
int change_begin_nospc(struct sb *sb, int cost, int limit)
{
	__change_begin(sb);

	if (nospc_add_cost(sb, cost, limit)) {
		__change_end(sb);
		return -ENOSPC;
	}

	return 0;
}

/* Start transaction with full ENOSPC check. */
static inline int change_begin_check(struct sb *sb, int cost, int limit)
{
	while (change_begin_nospc(sb, cost, limit)) {
		if (nospc_wait_and_check(sb, cost, limit))
			return -ENOSPC;
	}
	return 0;
}

/* For other than unlink/rmdir */
int change_begin(struct sb *sb, int cost)
{
	return change_begin_check(sb, cost, 0);
}

/* For unlink/rmdir */
int change_begin_unlink(struct sb *sb, int cost, bool orphaned)
{
	/* Use 75% of reserve. FIXME: magic number */
	int limit = min_reserve * 3 / 4;

	/* Add cost for orphan. */
	if (orphaned)
		cost += TUX3_COST_ORPHAN + TUX3_COST_ORPHAN_DEL;

	/* Can use reserve blocks too (negative limit) */
	return change_begin_check(sb, cost, -limit);
}

int change_end(struct sb *sb)
{
	int err = 0;

	__change_end(sb);

#ifdef TUX3_FLUSHER_SYNC
	err = try_flush_delta(sb);
#endif
	return err;
}
