/*
 * Iattr Fork (Copy-On-Write of inode attributes).
 *
 * Iattr fork is to reduce copy, it copies inode attributes once at most
 * per delta.
 *
 * If iattrs was dirtied by previous delta and stabled, the frontend
 * copy iattrs to backend slot from frontend slot before modify
 * iattrs, and remember delta number when dirtied.
 *
 * The backend checks the delta number of iattr. If delta number of
 * iattr == backend delta, the frontend didn't modify iattrs after
 * stabled, so backend uses the frontend slot. Then backend clears
 * delta number of iattr to tell the backend doesn't need fork
 * anymore.
 *
 * Otherwise, frontend forked the iatts to backend slot, so backend
 * uses the backend slot.
 *
 * Copyright (c) 2012-2014 OGAWA Hirofumi
 */

#include "tux3_fork.h"
#include "iattr.h"

TUX3_DEFINE_STATE_FNS(unsigned, iattr, IATTR_DIRTY,
		      IFLAGS_IATTR_BITS, IFLAGS_IATTR_SHIFT);

/* FIXME: can we consolidate tuxnode->lock usage with I_DIRTY and xattrdirty? */
/* FIXME: timestamps is updated without inode_lock, so racy. */

/* Caller must hold tuxnode->lock. */
static void idata_copy(struct inode *inode, struct tux3_iattr_data *idata)
{
	idata->i_mode		= inode->i_mode;
	idata->i_uid		= i_uid_read(inode);
	idata->i_gid		= i_gid_read(inode);
	idata->i_nlink		= inode->i_nlink;
	idata->i_size		= i_size_read(inode);
//	idata->i_atime		= inode->i_atime;
	idata->i_mtime		= inode->i_mtime;
	idata->i_ctime		= inode->i_ctime;
	idata->i_version	= inode_peek_iversion(inode);
	idata->generic		= iattr_encode_generic(inode);
}

/*
 * Inode attributes fork. (See comment on top of this source)
 *
 * NOTE: caller must call tux3_mark_inode_dirty() after
 * this. Otherwise, inode state will be remaining after flush, and
 * will confuses flusher in future.
 *
 * FIXME: this is better to call tux3_mark_inode_dirty() too?
 */
void tux3_iattrdirty(struct inode *inode)
{
	struct tux3_inode *tuxnode = tux_inode(inode);
	unsigned delta = tux3_inode_delta(inode);
	unsigned state = tuxnode->state;

	/* If dirtied on this delta, nothing to do */
	if (tux3_iattrsta_has_delta(state) &&
	    tux3_iattrsta_get_delta(state) == tux3_delta(delta))
		return;

	trace("inum %Lu, delta %u", tuxnode->inum, delta);

	spin_lock(&tuxnode->lock);
	state = tuxnode->state;
	if (S_ISREG(inode->i_mode) || tux3_iattrsta_has_delta(state)) {
		unsigned old_delta;

		/*
		 * For a regular file, and even if iattrs are clean,
		 * we have to provide stable idata for backend.
		 *
		 * Because backend may be committing data pages. If
		 * so, backend have to check idata->i_size, and may
		 * save dtree root. But previous delta doesn't have
		 * stable iattrs.
		 *
		 * So, this provides stable iattrs for regular file,
		 * even if previous delta is clean.
		 *
		 * Other types don't have this problem, because:
		 * - Never dirty iattr (e.g. volmap). IOW, iattrs are
		 *   always stable.
		 * - Or dirty iattr with data, e.g. directory updates
		 *   timestamp too with data blocks.
		 */
		if (S_ISREG(inode->i_mode) && !tux3_iattrsta_has_delta(state))
			old_delta = tux3_delta(delta - 1);
		else
			old_delta = tux3_iattrsta_get_delta(state);

		/* If delta is difference, iattrs was stabilized. Copy. */
		if (old_delta != tux3_delta(delta)) {
			struct tux3_iattr_data *idata =
				&tux3_inode_ddc(inode, old_delta)->idata;
			idata_copy(inode, idata);
		}
	}
	/* Update iattr state to current delta */
	tuxnode->state = tux3_iattrsta_update(state, delta);
	spin_unlock(&tuxnode->lock);
}

/* Caller must hold tuxnode->lock */
static void tux3_iattr_clear_dirty(struct tux3_inode *tuxnode)
{
	trace("inum %Lu", tuxnode->inum);
	tuxnode->state = tux3_iattrsta_clear(tuxnode->state);
}

/*
 * Peek i_size and dead states, keep dirty state as is.
 *
 * Caller must hold tuxnode->lock.
 */
static loff_t tux3_iattr_peek_i_size(struct inode *inode, unsigned *deleted,
				     unsigned delta)
{
	struct tux3_inode *tuxnode = tux_inode(inode);
	unsigned state;
	loff_t i_size;

	trace("inum %Lu, delta %u", tuxnode->inum, delta);

	/*
	 * If delta is same, iattrs are available in inode. If not,
	 * iattrs were forked.
	 */
	state = tuxnode->state;
	if (!tux3_iattrsta_has_delta(state) ||
	    tux3_iattrsta_get_delta(state) == tux3_delta(delta)) {
		/*
		 * If btree is only dirtied, or if dirty and no fork,
		 * use inode.
		 */
		i_size = i_size_read(inode);
	} else {
		/* If dirty and forked, use copy */
		struct tux3_iattr_data *idata =
			&tux3_inode_ddc(inode, delta)->idata;
		assert(idata->i_mode != TUX3_INVALID_IDATA);
		i_size = idata->i_size;
	}

	if (tux3_dead_read(state, delta))
		*deleted = 1;
	else
		*deleted = 0;

	return i_size;
}

/*
 * Read iattrs, then clear iattr dirty to tell no need to iattrfork
 * anymore if needed.
 *
 * Caller must hold tuxnode->lock.
 */
static void tux3_iattr_read_and_clear(struct inode *inode,
				      struct tux3_iattr_data *result,
				      unsigned delta)
{
	struct tux3_inode *tuxnode = tux_inode(inode);
	unsigned state;

	trace("inum %Lu, delta %u", tuxnode->inum, delta);

	/*
	 * If delta is same, iattrs are available in inode. If not,
	 * iattrs were forked.
	 */
	state = tuxnode->state;
	if (!tux3_iattrsta_has_delta(state) ||
	    tux3_iattrsta_get_delta(state) == tux3_delta(delta)) {
		/*
		 * If btree is only dirtied, or if dirty and no fork,
		 * use inode.
		 */
		idata_copy(inode, result);
		tuxnode->state = tux3_iattrsta_clear(state);
	} else {
		/* If dirty and forked, use copy */
		struct tux3_iattr_data *idata =
			&tux3_inode_ddc(inode, delta)->idata;
		assert(idata->i_mode != TUX3_INVALID_IDATA);
		*result = *idata;
	}

	/* For debugging, set invalid value after read */
	tux3_inode_ddc(inode, delta)->idata.i_mode = TUX3_INVALID_IDATA;
}
