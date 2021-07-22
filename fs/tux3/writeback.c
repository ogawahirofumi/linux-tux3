/*
 * Writeback for inodes.
 *
 * lock order:
 *     inode->i_lock
 *         tuxnode->lock
 *         sb->dirty_inodes_lock
 *
 * Copyright (c) 2012-2014 OGAWA Hirofumi
 */

#include "tux3.h"
#include "filemap_hole.h"

#ifndef trace
#define trace trace_on
#endif

/* FIXME: probably, we should rewrite with own buffer management. */

#if I_DIRTY != ((1 << 0) | (1 << 1) | (1 << 2))
#error "I_DIRTY was changed"
#endif
#if I_DIRTY_ALL != (I_DIRTY | I_DIRTY_TIME)
#error "I_DIRTY_ALL was changed"
#endif

/*
 * tuxnode->state usage from LSB:
 * - inode dirty flags per delta
 * - iattr delta
 * - xattr delta
 * - delete dirty delta
 * - btree dirty flag
 * - inode is orphaned flag
 * - inode is dead flag
 * - inode is flushed own timing flag
 */

/* I_DIRTY_SYNC, I_DIRTY_DATASYNC, and I_DIRTY_PAGES */
#define NUM_DIRTY_BITS		3U
/* Iattr fork dirty base */
#define IATTR_DIRTY		1U
/* Xattr fork dirty base */
#define XATTR_DIRTY		1U
/* Dead inode dirty base */
#define DEAD_DIRTY		1U
/* Bits usage of inode->state */
#define IFLAGS_DIRTY_BITS	(NUM_DIRTY_BITS * TUX3_MAX_DELTA)
#define IFLAGS_IATTR_BITS	(order_base_2(IATTR_DIRTY + TUX3_MAX_DELTA))
#define IFLAGS_XATTR_BITS	(order_base_2(XATTR_DIRTY + TUX3_MAX_DELTA))
#define IFLAGS_DEAD_BITS	(order_base_2(DEAD_DIRTY + TUX3_MAX_DELTA))
/* Bit shift for inode->state */
#define IFLAGS_IATTR_SHIFT	IFLAGS_DIRTY_BITS
#define IFLAGS_XATTR_SHIFT	(IFLAGS_IATTR_SHIFT + IFLAGS_IATTR_BITS)
#define IFLAGS_DEAD_SHIFT	(IFLAGS_XATTR_SHIFT + IFLAGS_XATTR_BITS)

/* btree root is modified from only backend, so no need per-delta flag */
#define TUX3_DIRTY_BTREE	(1U << 29)
/* the orphaned flag is set by only backend, so no need per-delta flag */
#define TUX3_INODE_ORPHANED	(1U << 30)
/* the dead flag is set by only backend, so no need per-delta flag */
#define TUX3_INODE_DEAD		(1U << 31)

#define NON_DIRTY_FLAGS		(TUX3_INODE_ORPHANED | TUX3_INODE_DEAD)

/*
 * Some inodes (e.g. bitmap inode) is always dirty, because it has
 * recursive of block allocation. So, if the inode was added to
 * wb.dirty_list, flusher always thinks there is dirty inode.
 *
 * So, this sets the dirty to inode to prevent to be added into
 * wb.dirty_list. With this, we are not bothered by always dirty
 * inodes.
 *
 * FIXME: hack, is there better way?
 */
void tux3_set_inode_always_dirty(struct inode *inode)
{
#ifdef __KERNEL__
	inode->i_state |= I_DIRTY_PAGES;
#endif
}

/*
 * Dirty flags helpers
 */
static inline unsigned tux3_dirty_shift(unsigned delta)
{
	return tux3_delta(delta) * NUM_DIRTY_BITS;
}

static inline unsigned tux3_dirty_mask(int flags, unsigned delta)
{
	return flags << tux3_dirty_shift(delta);
}

static inline unsigned tux3_dirty_flags(struct inode *inode, unsigned delta)
{
	unsigned state = tux_inode(inode)->state;
	unsigned ret;

	ret = (state >> tux3_dirty_shift(delta)) & I_DIRTY;
	ret |= state & TUX3_DIRTY_BTREE;
	return ret;
}

/* This is hook of __mark_inode_dirty() and called I_DIRTY_PAGES too */
void tux3_dirty_inode(struct inode *inode, int flags)
{
	struct sb *sb = tux_sb(inode->i_sb);
	struct tux3_inode *tuxnode = tux_inode(inode);
	unsigned delta = tux3_inode_delta(inode);
	struct sb_delta_dirty *s_ddc;
	struct inode_delta_dirty *i_ddc;
	int re_dirtied = 0;
	unsigned mask;

	/* FIXME: SB_LAZYTIME is not supported yet */
	flags &= I_DIRTY;
	mask = tux3_dirty_mask(flags, delta);

	if ((tuxnode->state & mask) == mask)
		return;

	/*
	 * If inode is TUX3_I_NO_FLUSH, it is handled by different
	 * cycle with sb->delta. So those can race.
	 *
	 * So, don't bother s_ddc->dirty_inodes by adding those inodes.
	 */
	if (tux3_inode_test_flag(TUX3_I_NO_FLUSH, inode))
		s_ddc = NULL;
	else {
		s_ddc = tux3_sb_ddc(sb, delta);
		i_ddc = tux3_inode_ddc(inode, delta);
	}

	spin_lock(&tuxnode->lock);
	if ((tuxnode->state & mask) != mask) {
		tuxnode->state |= mask;

		if (s_ddc) {
			spin_lock(&sb->dirty_inodes_lock);
			if (list_empty(&i_ddc->dirty_list)) {
				list_add_tail(&i_ddc->dirty_list,
					      &s_ddc->dirty_inodes);
				/* The inode was re-dirtied while flushing. */
				re_dirtied = (inode->i_state & I_DIRTY);
			}
			spin_unlock(&sb->dirty_inodes_lock);
		}
	}
	spin_unlock(&tuxnode->lock);

	/*
	 * Update ->i_io_list and ->dirtied_when if needed.
	 *
	 * __mark_inode_dirty() doesn't know about delta boundary (we don't
	 * clear I_DIRTY before flush, in order to prevent the inode to be
	 * freed). So, if inode was re-dirtied for frontend delta while
	 * flushing old delta, ->dirtied_when may not be updated by
	 * __mark_inode_dirty() forever.
	 *
	 * Although we don't use ->dirtied_when, bdi flusher uses
	 * ->dirtied_when to decide flush timing, so we have to update
	 * ->dirtied_when ourself.
	 */
	if (re_dirtied)
		inode_writeback_touch(inode);
}

/*
 * Called from backend only to mark btree as dirty. (usually from the
 * path of flushing buffers)
 */
void tux3_mark_btree_dirty(struct btree *btree)
{
	if (btree != itree_btree(btree->sb) &&
	    btree != otree_btree(btree->sb)) {
		struct tux3_inode *tuxnode = tux_inode(btree_inode(btree));

		spin_lock(&tuxnode->lock);
		tuxnode->state |= TUX3_DIRTY_BTREE;
		spin_unlock(&tuxnode->lock);
	}
}

#include "writeback_inodedelete.c"
#include "writeback_iattrfork.c"
#include "writeback_xattrfork.c"

/*
 * Clear dirty flags for delta (caller must hold inode->i_lock/tuxnode->lock).
 *
 * Note: This can race with *_mark_inode_dirty().
 *
 *        cpu0                                 cpu1
 * __tux3_mark_inode_dirty()
 *     tux3_dirty_inode()
 *         mark delta dirty
 *                                      tux3_clear_dirty_inode_nolock()
 *                                           clear core dirty and delta
 *     __mark_inode_dirty()
 *         mark core dirty
 *
 * For this race, we can't use this to clear dirty for frontend delta
 * (exception is the points which has no race like umount).
 */
static void tux3_clear_dirty_inode_nolock(struct inode *inode, unsigned delta,
					  int frontend)
{
	struct sb *sb = tux_sb(inode->i_sb);
	struct tux3_inode *tuxnode = tux_inode(inode);
	unsigned mask, old_dirty;

	/* FIXME: SB_LAZYTIME is not supported yet */
	mask = tux3_dirty_mask(I_DIRTY, delta);

	old_dirty = tuxnode->state & (TUX3_DIRTY_BTREE | mask);
	/* Clear dirty flags for delta */
	tuxnode->state &= ~(TUX3_DIRTY_BTREE | mask);

	/* Remove inode from list */
	if (old_dirty) {
		/* Only if called from frontend, we need to lock */
		if (frontend)
			spin_lock(&sb->dirty_inodes_lock);

		list_del_init(&tux3_inode_ddc(inode, delta)->dirty_list);

		if (frontend)
			spin_unlock(&sb->dirty_inodes_lock);
	}

	/* Update state if inode isn't dirty anymore */
	if (!(tuxnode->state & ~NON_DIRTY_FLAGS))
		inode->i_state &= ~I_DIRTY;
}

/* Clear dirty flags for delta */
#ifdef __KERNEL__
/* FIXME: userspace uses this */
static
#endif
void __tux3_clear_dirty_inode(struct inode *inode, unsigned delta)
{
	struct tux3_inode *tuxnode = tux_inode(inode);

	spin_lock(&inode->i_lock);
	spin_lock(&tuxnode->lock);
	tux3_clear_dirty_inode_nolock(inode, delta, 0);
	spin_unlock(&tuxnode->lock);
	spin_unlock(&inode->i_lock);

	inode_writeback_done(inode);
}

/*
 * Clear dirty flags for frontend delta.
 * Note: see comment of tux3_clear_dirty_inode_nolock)
 */
void tux3_clear_dirty_inode(struct inode *inode)
{
	struct tux3_inode *tuxnode = tux_inode(inode);

	spin_lock(&inode->i_lock);
	spin_lock(&tuxnode->lock);
	tux3_iattr_clear_dirty(tuxnode);
	tux3_clear_dirty_inode_nolock(inode, tux3_inode_delta(inode), 1);
	spin_unlock(&tuxnode->lock);
	spin_unlock(&inode->i_lock);

	inode_writeback_done(inode);
}

void __tux3_mark_inode_dirty(struct inode *inode, int flags)
{
	/* Call tux3_dirty_inode() for I_DIRTY_PAGES too */
	if ((flags & I_DIRTY) == I_DIRTY_PAGES)
		tux3_dirty_inode(inode, flags);

	__mark_inode_dirty(inode, flags);
}

/*
 * Mark buffer as dirty to flush at delta flush.
 *
 * This is used with pagefork_for_blockdirty().  If caller uses this,
 * caller must hold lock_page().
 */
void __tux3_mark_buffer_dirty(struct buffer_head *buffer, unsigned delta)
{
	struct inode *inode;

	/*
	 * Very *carefully* optimize the it-is-already-dirty case.
	 *
	 * Don't let the final "is it dirty" escape to before we
	 * perhaps modified the buffer.
	 */
	if (buffer_dirty(buffer)) {
		smp_mb();
		if (buffer_dirty(buffer))
			return;
	}

	inode = buffer_inode(buffer);
#ifdef __KERNEL__
	assert(tux3_inode_test_flag(TUX3_I_NO_DELTA, inode) ||
	       PageLocked(buffer->b_page));
#endif

	if (tux3_set_buffer_dirty(mapping(inode), buffer, delta))
		__tux3_mark_inode_dirty(inode, I_DIRTY_PAGES);
}

/*
 * Mark buffer as dirty to flush at delta flush.
 *
 * Specified buffer must be for volmap/logmap (i.e. no buffer fork, and
 * page->mapping is valid). Otherwise this will race with buffer fork.
 */
void tux3_mark_buffer_dirty(struct buffer_head *buffer)
{
	struct inode *inode = buffer_inode(buffer);
	assert(tux3_inode_test_flag(TUX3_I_NO_DELTA, inode));
	__tux3_mark_buffer_dirty(buffer, TUX3_INIT_DELTA);
}

/*
 * Mark buffer as dirty to flush at unify flush
 *
 * Specified buffer must be for volmap (i.e. no buffer fork, and
 * page->mapping is valid). Otherwise this will race with buffer fork.
 */
void tux3_mark_buffer_unify(struct buffer_head *buffer)
{
	struct sb *sb;
	struct inode *inode;

	/*
	 * Very *carefully* optimize the it-is-already-dirty case.
	 *
	 * Don't let the final "is it dirty" escape to before we
	 * perhaps modified the buffer.
	 */
	if (buffer_dirty(buffer)) {
		smp_mb();
		if (buffer_dirty(buffer))
			return;
	}

	inode = buffer_inode(buffer);
	sb = tux_sb(inode->i_sb);
	assert(inode == sb->volmap); /* must be volmap */

	tux3_set_buffer_dirty_list(mapping(inode), buffer, sb->unify,
				   &sb->unify_buffers);
	/*
	 * We don't call __tux3_mark_inode_dirty() here, because unify
	 * is not flushed per-delta. So, marking volmap as dirty just
	 * bothers us.
	 *
	 * Instead, we call __tux3_mark_inode_dirty() when process
	 * unify buffers in unify_log().
	 */
}

/* Caller must hold tuxnode->lock, or replay (no frontend) */
void tux3_mark_inode_orphan(struct tux3_inode *tuxnode)
{
	tuxnode->state |= TUX3_INODE_ORPHANED;
}

int tux3_inode_is_orphan(struct tux3_inode *tuxnode)
{
	return !!(tuxnode->state & TUX3_INODE_ORPHANED);
}

static loff_t tux3_state_peek_i_size(struct inode *inode, unsigned *deleted,
				     unsigned delta)
{
	struct tux3_inode *tuxnode = tux_inode(inode);
	loff_t i_size;

	spin_lock(&tuxnode->lock);
	/* Get iattr data */
	i_size = tux3_iattr_peek_i_size(inode, deleted, delta);
	spin_unlock(&tuxnode->lock);

	return i_size;
}

static void tux3_state_read_and_clear(struct inode *inode,
				      struct tux3_iattr_data *idata,
				      unsigned *orphaned, unsigned *deleted,
				      unsigned delta)
{
	struct tux3_inode *tuxnode = tux_inode(inode);

	spin_lock(&tuxnode->lock);

	/* Get iattr data */
	tux3_iattr_read_and_clear(inode, idata, delta);

	/* Check orphan state */
	*orphaned = 0;
	if (idata->i_nlink == 0 && !(tuxnode->state & TUX3_INODE_ORPHANED)) {
		/* This inode was orphaned in this delta */
		*orphaned = 1;
		tux3_mark_inode_orphan(tuxnode);
	}

	/* Check whether inode has to delete */
	tux3_dead_read_and_clear(inode, deleted, delta);

	spin_unlock(&tuxnode->lock);
}

static inline int tux3_flush_buffers(struct inode *inode,
				     struct tux3_iattr_data *idata,
				     unsigned deleted,
				     unsigned delta, unsigned int req_flags)
{
	struct list_head *dirty_buffers = tux3_dirty_buffers(inode, delta);
	int err;

	/* FIXME: error handling */

	/*
	 * If inode was marked as dead, we don't need to flush holes.
	 * Instead of this, tux3_purge_inode() will remove holes later.
	 */
	if (!deleted) {
		/* Apply hole extents before page caches */
		err = tux3_flush_hole(inode, delta);
		if (err)
			return err;
	} else {
		/*
		 * If inode was marked as dead, we don't need to flush data.
		 * To skip to flush data, this sets fake i_size=0.
		 */
		idata->i_size = 0;
	}

	/* Apply page caches */
	return flush_list(inode, idata, dirty_buffers, req_flags);
}

/*
 * Flush inode.
 *
 * The inode dirty state keeps until finish I/O to prevent inode
 * reclaim. Because we don't wait writeback on evict_inode(), and
 * instead we keeps the inode while writeback is running.
 */
static int tux3_flush_inode_data(struct inode *inode, unsigned delta,
				 unsigned int req_flags)
{
	struct tux3_iattr_data idata;
	unsigned deleted;

	idata.i_size = tux3_state_peek_i_size(inode, &deleted, delta);
	return tux3_flush_buffers(inode, &idata, deleted, delta, req_flags);
}

int tux3_flush_inode(struct inode *inode, unsigned delta,
		     unsigned int req_flags)
{
	struct tux3_iattr_data idata;
	unsigned dirty = 0, orphaned, deleted;
	int ret = 0, err;

	/*
	 * Read the stabled inode attributes and state for this delta,
	 * then tell we read already.
	 */
	tux3_state_read_and_clear(inode, &idata, &orphaned, &deleted, delta);

	trace("inum %Lu, idata %p, orphaned %d, deleted %d, delta %u",
	      tux_inode(inode)->inum, &idata, orphaned, deleted, delta);

	if (!deleted) {
		/* If orphaned on this delta, add orphan */
		if (orphaned) {
			err = tux3_make_orphan_add(inode);
			if (err && !ret)
				ret = err;
		}
	} else {
		/* If orphaned on past delta, delete orphan */
		if (!orphaned) {
			err = tux3_make_orphan_del(inode);
			if (err && !ret)
				ret = err;
		}

		/*
		 * Remove from hash before deleting the inode from itree.
		 * Otherwise, when inum is reused, this inode will be
		 * unexpectedly grabbed via hash.
		 */
		remove_inode_hash(inode);

		/* If inode was deleted and referencer was gone, delete inode */
		err = tux3_purge_inode(inode, &idata, delta);
		if (err && !ret)
			ret = err;
	}

	/*
	 * Get flags after tux3_flush_buffers() to check TUX3_DIRTY_BTREE.
	 * If inode is dead, we don't need to save inode.
	 */
	if (!deleted)
		dirty = tux3_dirty_flags(inode, delta);

	if (dirty & (TUX3_DIRTY_BTREE | I_DIRTY_INODE)) {
		err = tux3_save_inode(inode, &idata, delta);
		if (err && !ret)
			ret = err;
	}

	/* FIXME: In the error path, dirty state would still be
	 * remaining, we have to do something. */

	return ret;
}

/*
 * tux3_flush_inode() for TUX3_I_NO_FLUSH.
 *
 * If inode is TUX3_I_NO_FLUSH, those can clear inode dirty state
 * immediately. Because those inodes is pinned until umount.
*/
int tux3_flush_inode_internal(struct inode *inode, unsigned delta,
			      unsigned int req_flags)
{
	int err;

	assert(tux3_inode_test_flag(TUX3_I_NO_FLUSH, inode));
	assert(atomic_read(&inode->i_count) >= 1);

	/*
	 * Check dirty state roughly (possibly false positive. True
	 * dirty state is in tuxnode->state and per-delta dirty
	 * buffers list) to avoid lock overhead.
	 */
	if (!(inode->i_state & I_DIRTY))
		return 0;

	err = tux3_flush_inode_data(inode, delta, req_flags);
	err = tux3_flush_inode(inode, delta, req_flags);
	/* FIXME: error handling */
	__tux3_clear_dirty_inode(inode, delta);

	return err;
}

static int inode_inum_cmp(void *priv, const struct list_head *a,
			  const struct list_head *b)
{
	struct tux3_inode *ta, *tb;
	struct inode_delta_dirty *i_ddc;
	unsigned delta = *(unsigned *)priv;
	inum_t inum_a, inum_b;

	i_ddc = list_entry(a, struct inode_delta_dirty, dirty_list);
	ta = i_ddc_to_inode(i_ddc, delta);
	i_ddc = list_entry(b, struct inode_delta_dirty, dirty_list);
	tb = i_ddc_to_inode(i_ddc, delta);

	inum_a = ((inum_t)!S_ISDIR(ta->vfs_inode.i_mode) << 63) | ta->inum;
	inum_b = ((inum_t)!S_ISDIR(tb->vfs_inode.i_mode) << 63) | tb->inum;
	if (inum_a < inum_b)
		return -1;
	else if (inum_a > inum_b)
		return 1;
	return 0;
}

int tux3_flush_inodes(struct sb *sb, unsigned delta)
{
	struct sb_delta_dirty *s_ddc = tux3_sb_ddc(sb, delta);
	struct list_head *dirty_inodes = &s_ddc->dirty_inodes;
	struct inode_delta_dirty *i_ddc, *safe;
	inum_t private;
	int err;

	/* ->dirty_inodes owned by backend. No need to lock here */

	/* Sort by tuxnode->inum. FIXME: do we want to sort? */
	list_sort(&delta, dirty_inodes, inode_inum_cmp);

	policy_inode_init(&private);
	list_for_each_entry_safe(i_ddc, safe, dirty_inodes, dirty_list) {
		struct tux3_inode *tuxnode = i_ddc_to_inode(i_ddc, delta);
		struct inode *inode = &tuxnode->vfs_inode;

		policy_inode(inode, &private);
		assert(!tux3_inode_test_flag(TUX3_I_NO_FLUSH, inode));

		err = tux3_flush_inode_data(inode, delta, 0);
		if (err)
			goto error;
	}
	list_for_each_entry_safe(i_ddc, safe, dirty_inodes, dirty_list) {
		struct tux3_inode *tuxnode = i_ddc_to_inode(i_ddc, delta);
		struct inode *inode = &tuxnode->vfs_inode;

		assert(!tux3_inode_test_flag(TUX3_I_NO_FLUSH, inode));

		err = tux3_flush_inode(inode, delta, 0);
		if (err)
			goto error;
	}

	return 0;

error:
	/* FIXME: what to do for dirty_inodes on error path */
	return err;
}

int tux3_has_dirty_inodes(struct sb *sb, unsigned delta)
{
	struct sb_delta_dirty *s_ddc = tux3_sb_ddc(sb, delta);
	/* If no dirty inodes, internal inodes should clean too. */
	assert(!list_empty(&s_ddc->dirty_inodes) ||
	       (list_empty(&s_ddc->dirty_inodes) &&
		!(sb->atable->i_state & I_DIRTY)));
#if 0
		&& !(sb->vtable->i_state & I_DIRTY)));
#endif
	return !list_empty(&s_ddc->dirty_inodes);
}

/*
 * Clear inode dirty state after flush.
 */
void tux3_clear_dirty_inodes(struct sb *sb, unsigned delta)
{
	struct sb_delta_dirty *s_ddc = tux3_sb_ddc(sb, delta);
	struct list_head *dirty_inodes = &s_ddc->dirty_inodes;
	struct inode_delta_dirty *i_ddc, *safe;

	list_for_each_entry_safe(i_ddc, safe, dirty_inodes, dirty_list) {
		struct tux3_inode *tuxnode = i_ddc_to_inode(i_ddc, delta);
		struct inode *inode = &tuxnode->vfs_inode;

		assert(!tux3_inode_test_flag(TUX3_I_NO_FLUSH, inode));

		/*
		 * iput_final() doesn't add inode to LRU list if I_DIRTY.
		 * Grab refcount to tell inode state was changed to iput().
		 *
		 * FIXME: iget and iput set I_REFERENCED, but we would
		 * not want to set I_REFERENCED for clearing dirty.
		 */
		spin_lock(&inode->i_lock);
		iget_if_dirty(inode);
		spin_unlock(&inode->i_lock);

		__tux3_clear_dirty_inode(inode, delta);

		iput(inode);
	}

	assert(list_empty(dirty_inodes)); /* someone redirtied own inode? */
}

unsigned tux3_check_tuxinode_state(struct inode *inode)
{
	return tux_inode(inode)->state & ~NON_DIRTY_FLAGS;
}
