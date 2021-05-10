/*
 * Inode operations.
 *
 * Copyright (c) 2008-2014 Daniel Phillips
 * Copyright (c) 2008-2014 OGAWA Hirofumi
 */

#include "tux3.h"
#include "filemap_hole.h"
#include "ileaf.h"
#include "iattr.h"

#ifndef trace
#define trace trace_on
#endif

static void tux_setup_inode(struct inode *inode);

static inline void tux_set_inum(struct inode *inode, inum_t inum)
{
#ifdef __KERNEL__
	inode->i_ino = inum;
#endif
	tux_inode(inode)->inum = inum;
}

struct inode *tux_new_volmap(struct sb *sb)
{
	struct inode *inode = new_inode(vfs_sb(sb));
	if (inode) {
		tux_set_inum(inode, TUX_VOLMAP_INO);
		tux_setup_inode(inode);
		insert_inode_hash(inode);
	}
	return inode;
}

struct inode *tux_new_logmap(struct sb *sb)
{
	struct inode *inode = new_inode(vfs_sb(sb));
	if (inode) {
		tux_set_inum(inode, TUX_LOGMAP_INO);
		tux_setup_inode(inode);
		insert_inode_hash(inode);
	}
	return inode;
}

static void tux_inode_init_owner(struct inode *inode, const struct inode *dir,
				 struct tux_iattr *iattr)
{
	umode_t mode = iattr->mode;

	inode->i_uid = iattr->uid;
	if (dir && (dir->i_mode & S_ISGID)) {
		inode->i_gid = dir->i_gid;
		if (S_ISDIR(mode))
			mode |= S_ISGID;
	} else
		inode->i_gid = iattr->gid;
	inode->i_mode = mode;
}

struct inode *tux_new_inode(struct sb *sb, struct inode *dir,
			    struct tux_iattr *iattr)
{
	inum_t parent_inum = dir ? tux_inode(dir)->inum : TUX_ROOTDIR_INO;
	struct inode *inode;

	inode = new_inode(vfs_sb(sb));
	if (!inode)
		return NULL;

	tux_inode_init_owner(inode, dir, iattr);

	inode->i_mtime = inode->i_ctime = inode->i_atime = current_time(inode);
	switch (inode->i_mode & S_IFMT) {
	case S_IFBLK:
	case S_IFCHR:
		/* vfs, trying to be helpful, will rewrite the field */
		inode->i_rdev = iattr->rdev;
		break;
	case S_IFDIR:
		inc_nlink(inode);
		tux_inode(inode)->parent_inum = parent_inum;
		break;
	}

	init_btree(&tux_inode(inode)->btree, sb, no_root, &dtree_ops);

	/* Just for debug, will rewrite by alloc_inum() */
	tux_set_inum(inode, TUX_INVALID_INO);

	return inode;
}

/* must hold itree->lock */
static inline void freeinodes_dec(struct sb *sb, const inum_t inum)
{
	/*
	 * If inum is not reserved area, account it. If inum is
	 * reserved area, inode might not be written into itree. So,
	 * we don't include the reserved area into dynamic accounting.
	 * FIXME: what happen if snapshot was introduced?
	 */
	if (inum >= TUX_NORMAL_INO) {
		assert(sb->freeinodes > TUX_NORMAL_INO);
		sb->freeinodes--;
	}
}

/* must hold itree->lock */
static inline void freeinodes_inc(struct sb *sb, const inum_t inum)
{
	/*
	 * If inum is not reserved area, account it.
	 * FIXME: what happen if snapshot was introduced?
	 */
	if (inum >= TUX_NORMAL_INO) {
		assert(sb->freeinodes < MAX_INODES);
		sb->freeinodes++;
	}
}

/*
 * Deferred ileaf update for inode number allocation
 */

#include "inode_defer.c"

/* must hold itree->btree.lock */
static inline int is_defer_alloc_inum(struct inode *inode)
{
	return tux3_inode_test_flag(TUX3_I_DEFER_INUM, inode);
}

/* must hold itree->btree.lock */
static int add_defer_alloc_inum(struct inode *inode)
{
	/* FIXME: need to reserve space (ileaf/bnodes) for this inode? */
	struct sb *sb = tux_sb(inode->i_sb);
	inum_t inum = tux_inode(inode)->inum;
	int err = tux3_idefer_add(sb->idefer_map, inum);
	if (!err) {
		tux3_inode_set_flag(TUX3_I_DEFER_INUM, inode);
		/* Decrement the free inodes */
		freeinodes_dec(sb, inum);
	}
	return err;
}

/* must hold itree->btree.lock. FIXME: spinlock is enough? */
static void del_defer_alloc_inum(struct inode *inode)
{
	struct sb *sb = tux_sb(inode->i_sb);
	tux3_idefer_del(sb->idefer_map, tux_inode(inode)->inum);
	tux3_inode_clear_flag(TUX3_I_DEFER_INUM, inode);
}

void cancel_defer_alloc_inum(struct inode *inode)
{
	struct sb *sb = tux_sb(inode->i_sb);

	down_write(&itree_btree(sb)->lock);	/* FIXME: spinlock is enough? */
	if (is_defer_alloc_inum(inode)) {
		freeinodes_inc(sb, tux_inode(inode)->inum);
		del_defer_alloc_inum(inode);
	}
	up_write(&itree_btree(sb)->lock);
}

/*
 * Inode btree expansion algorithm
 *
 * First probe for the inode goal.  This retreives the rightmost leaf that
 * contains an inode less than or equal to the goal.  (We could in theory avoid
 * retrieving any leaf at all in some cases if we observe that the the goal must
 * fall into an unallocated gap between two index keys, for what that is worth.
 * Probably not very much.)
 *
 * If not at end then next key is greater than goal.  This block has the highest
 * ibase less than or equal to goal.  Ibase should be equal to btree key, so
 * assert.  Search block even if ibase is way too low.  If goal comes back equal
 * to next_key then there is no room to create more inodes in it, so advance to
 * the next block and repeat.
 *
 * Otherwise, expand the inum goal that came back.  If ibase was too low to
 * create the inode in that block then the low level split will fail and expand
 * will create a new ileaf block with ibase at the goal.  We round the
 * goal down to some binary multiple in ileaf_split to reduce the chance of
 * creating ileaf blocks with only a small number of inodes.  (Actually
 * we should only round down the split point, not the returned goal.)
 */

static int find_free_inum(struct cursor *cursor, inum_t goal, inum_t *allocated)
{
	int ret;

#ifndef __KERNEL__ /* FIXME: kill this, only mkfs path needs this */
	/* If this is not mkfs path, it should have itree root */
	if (!has_root(cursor->btree)) {
		*allocated = goal;
		return 0;
	}
#endif

	ret = btree_probe(cursor, goal);
	if (ret)
		return ret;

	/* FIXME: need better allocation policy */

	/*
	 * Find free inum from goal, and wrapped to TUX_NORMAL_INO if
	 * not found. This prevent to use less than TUX_NORMAL_INO if
	 * reserved ino was not specified explicitly.
	 */
	ret = btree_traverse(cursor, goal, TUXKEY_LIMIT, ileaf_find_free,
			     allocated);
	if (ret < 0)
		goto out;
	if (ret > 0) {
		/* Found free inum */
		ret = 0;
		goto out;
	}

	if (TUX_NORMAL_INO < goal) {
		u64 len = goal - TUX_NORMAL_INO;

		ret = btree_traverse(cursor, TUX_NORMAL_INO, len,
				     ileaf_find_free, allocated);
		if (ret < 0)
			goto out;
		if (ret > 0) {
			/* Found free inum */
			ret = 0;
			goto out;
		}
	}

	/* Couldn't find free inum */
	ret = -ENOSPC;

out:
	release_cursor(cursor);

	return ret;
}

static int tux_test(struct inode *inode, void *data)
{
	return tux_inode(inode)->inum == *(inum_t *)data;
}

static int tux_set(struct inode *inode, void *data)
{
	tux_set_inum(inode, *(inum_t *)data);
	return 0;
}

static int alloc_inum(struct inode *inode, inum_t goal)
{
	struct sb *sb = tux_sb(inode->i_sb);
	struct btree *itree = itree_btree(sb);
	struct cursor *cursor;
	int err = 0;

	down_write(&itree->lock);
	cursor = alloc_cursor(itree, 0);
	if (!cursor) {
		err = -ENOMEM;
		goto error;
	}

	while (1) {
		inum_t orig;

		err = find_free_inum(cursor, goal, &goal);
		if (err)
			goto error;

		/* Skip deferred allocate inums */
		orig = goal;
		goal = tux3_idefer_find_free(sb->idefer_map, orig);
		if (orig == goal)
			break;
		/* If goal marked as deferred inums, retry from itree */
	}

	tux_set_inum(inode, goal);
	err = add_defer_alloc_inum(inode);
	if (err)
		goto error;

error:
	up_write(&itree->lock);
	free_cursor(cursor);

	return err;
}

static void tux_finalize_new_inode_failed(struct inode *inode)
{
	/*
	 * If inode was initialized and hashed already, it would be
	 * better to use deferred deletion path.
	 */
	assert(inode_unhashed(inode));

	cancel_defer_alloc_inum(inode);

	/* We drop the inode early without delete process in flusher */
	make_bad_inode(inode);

	clear_nlink(inode);
	iput(inode);
}

static int tux_finalize_new_inode(struct inode *inode, inum_t goal)
{
	inum_t inum;
	int err;

	err = alloc_inum(inode, goal);
	if (err)
		goto error;

	/* Final initialization of inode */
	tux_setup_inode(inode);

	inum = tux_inode(inode)->inum;
	if (insert_inode_locked4(inode, inum, tux_test, &inum) < 0) {
		/* Can be nfsd race happened, or fs corruption. */
		tux3_warn(tux_sb(inode->i_sb), "inode insert error: inum %Lx",
			  inum);
		err = -EIO;
		goto error;
	}

	/* The inode was hashed, we can use deferred deletion from here */

	/*
	 * The unhashed inode ignores mark_inode_dirty(), so it should
	 * be called after insert_inode_hash().
	 */
	tux3_iattrdirty(inode);
	tux3_mark_inode_dirty(inode);

	return 0;

error:
	tux_finalize_new_inode_failed(inode);
	return err;
}

struct inode *tux_create_inode(struct inode *dir, loff_t dir_pos,
			       struct tux_iattr *iattr)
{
	struct sb *sb = tux_sb(dir->i_sb);
	struct inode *inode;
	inum_t goal;
	int err;

	inode = tux_new_inode(sb, dir, iattr);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	goal = policy_inum(dir, dir_pos, iattr);
	err = tux_finalize_new_inode(inode, goal);
	if (err)
		return ERR_PTR(err);

	sb->nextinum = tux_inode(inode)->inum + 1; /* FIXME: racy */

	return inode;
}

/* Allocate inode with specific inum allocation policy */
struct inode *tux_create_specific_inode(struct sb *sb, struct inode *dir,
					inum_t inum, struct tux_iattr *iattr)
{
	struct inode *inode;
	int err;

	inode = tux_new_inode(sb, dir, iattr);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	err = tux_finalize_new_inode(inode, inum);
	if (err)
		return ERR_PTR(err);

	return inode;
}

static int open_inode(struct inode *inode)
{
	struct sb *sb = tux_sb(inode->i_sb);
	struct btree *itree = itree_btree(sb);
	struct cursor *cursor;
	int err;

	down_read(&itree->lock);
	cursor = alloc_cursor(itree, 0);
	if (!cursor) {
		err = -ENOMEM;
		goto out;
	}

	err = btree_probe(cursor, tux_inode(inode)->inum);
	if (err)
		goto out;

	/* Read inode attribute from inode btree */
	struct ileaf_req rq = {
		.key = {
			.start	= tux_inode(inode)->inum,
			.len	= 1,
		},
		.data	= inode,
	};
	err = btree_read(cursor, &rq.key);
	if (!err)
		tux_setup_inode(inode);

	release_cursor(cursor);
out:
	up_read(&itree->lock);
	free_cursor(cursor);

	return err;
}

struct inode *__tux3_iget(struct sb *sb, inum_t inum)
{
	struct inode *inode;
	int err;

	inode = iget5_locked(vfs_sb(sb), inum, tux_test, tux_set, &inum);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW))
		return inode;

	err = open_inode(inode);
	if (err) {
		iget_failed(inode);
		return ERR_PTR(err);
	}
	unlock_new_inode(inode);
	return inode;
}

struct inode *tux3_ilookup_nowait(struct sb *sb, inum_t inum)
{
	return ilookup5_nowait(vfs_sb(sb), inum, tux_test, &inum);
}

struct inode *tux3_ilookup(struct sb *sb, inum_t inum)
{
	return ilookup5(vfs_sb(sb), inum, tux_test, &inum);
}

static int save_inode(struct inode *inode, struct tux3_iattr_data *idata,
		      unsigned delta)
{
	struct sb *sb = tux_sb(inode->i_sb);
	struct btree *itree = itree_btree(sb);
	inum_t inum = tux_inode(inode)->inum;
	int err = 0;

	trace("save inode 0x%Lx", inum);

#ifndef __KERNEL__
	/* FIXME: kill this, only mkfs path needs this */
	/* FIXME: this should be merged to btree_expand()? */
	down_write(&itree->lock);
	if (!has_root(itree))
		err = btree_alloc_empty(itree);
	up_write(&itree->lock);
	if (err)
		return err;
#endif

	struct cursor *cursor = alloc_cursor(itree, 1); /* +1 for new depth */
	if (!cursor)
		return -ENOMEM;

	down_write(&cursor->btree->lock);
	err = btree_probe(cursor, inum);
	if (err)
		goto out;
	/* paranoia check */
	if (!is_defer_alloc_inum(inode))
		assert(ileaf_inum_exists(itree, bufdata(cursor_leafbuf(cursor)), inum));

	/* Write inode attributes to inode btree */
	struct iattr_req_data iattr_data = {
		.idata	= idata,
		.btree	= &tux_inode(inode)->btree,
		.inode	= inode,
		/* .present = {}, zero initialize */
	};
	struct ileaf_req rq = {
		.key = {
			.start	= inum,
			.len	= 1,
		},
		.data		= &iattr_data,
	};
	err = btree_write(cursor, &rq.key);
	if (err)
		goto error_release;

	/*
	 * If inode is newly added into itree, account to on-disk usedinodes.
	 * ->usedinodes is used only by backend, no need lock.
	 * FIXME: what happen if snapshot was introduced?
	 */
	if (is_defer_alloc_inum(inode)) {
		if (inum >= TUX_NORMAL_INO) {
			assert(be64_to_cpu(sb->super.usedinodes) < MAX_INODES);
			be64_add_cpu(&sb->super.usedinodes, 1);
		}
		del_defer_alloc_inum(inode);
	}

error_release:
	release_cursor(cursor);
out:
	up_write(&cursor->btree->lock);
	free_cursor(cursor);
	return err;
}

int tux3_save_inode(struct inode *inode, struct tux3_iattr_data *idata,
		    unsigned delta)
{
	/* Those inodes must not be marked as I_DIRTY_SYNC/DATASYNC. */
	assert(tux_inode(inode)->inum != TUX_VOLMAP_INO &&
	       tux_inode(inode)->inum != TUX_LOGMAP_INO &&
	       tux_inode(inode)->inum != TUX_INVALID_INO);
	switch (tux_inode(inode)->inum) {
	case TUX_BITMAP_INO:
	case TUX_COUNTMAP_INO:
	case TUX_VTABLE_INO:
	case TUX_ATABLE_INO:
		/* FIXME: assert(only btree should be changed); */
		break;
	}
	return save_inode(inode, idata, delta);
}

/* FIXME: we wait page under I/O though, we would like to fork it instead */
static int tux3_truncate(struct inode *inode, loff_t newsize)
{
	/* FIXME: expanding size is not tested */
#ifdef __KERNEL__
	const unsigned boundary = PAGE_SIZE;
#else
	const unsigned boundary = tux_sb(inode->i_sb)->blocksize;
#endif
	loff_t oldsize = inode->i_size;
	loff_t holebegin;
	int is_expand, err;

	if (newsize == oldsize)
		return 0;

	err = 0;
	is_expand = newsize > oldsize;

	if (!is_expand) {
		err = tux3_truncate_partial_block(inode, newsize);
		if (err)
			goto error;
	}

	/* Change i_size, then clean buffers */
	tux3_iattrdirty(inode);
	i_size_write(inode, newsize);
	if (is_expand)
		pagecache_isize_extended(inode, oldsize, newsize);
	/* Roundup. Partial page is handled by tux3_truncate_partial_block() */
	holebegin = round_up(newsize, boundary);
	if (newsize <= holebegin) {	/* Check overflow */
		/* FIXME: truncate_inode_pages_range() is broken if
		 * passed LLONG_MAX to lend. */
		truncate_pagecache(inode, holebegin);
	}

	if (!is_expand)
		err = tux3_add_truncate_hole(inode, newsize);

	inode->i_mtime = inode->i_ctime = current_time(inode);
	tux3_mark_inode_dirty(inode);
error:

	return err;
}

/* Remove inode from itree */
static int purge_inode(struct inode *inode)
{
	struct sb *sb = tux_sb(inode->i_sb);
	struct btree *itree = itree_btree(sb);
	inum_t inum = tux_inode(inode)->inum;

	down_write(&itree->lock);	/* FIXME: spinlock is enough? */

	/* Increment the free inodes */
	freeinodes_inc(sb, inum);

	if (is_defer_alloc_inum(inode)) {
		del_defer_alloc_inum(inode);
		up_write(&itree->lock);
		return 0;
	}
	up_write(&itree->lock);

	/*
	 * If inode is deleted from itree, account to on-disk usedinodes.
	 * ->usedinodes is used only by backend, no need lock.
	 * FIXME: what happen if snapshot was introduced?
	 */
	if (inum >= TUX_NORMAL_INO) {
		assert(be64_to_cpu(sb->super.usedinodes) > TUX_NORMAL_INO);
		be64_add_cpu(&sb->super.usedinodes, -1);
	}

	/* Remove inum from inode btree */
	return btree_chop(itree, tux_inode(inode)->inum, 1);
}

static int tux3_truncate_blocks(struct inode *inode, loff_t newsize)
{
	struct sb *sb = tux_sb(inode->i_sb);
	tuxkey_t index = (newsize + sb->blockmask) >> sb->blockbits;

	return dtree_chop(&tux_inode(inode)->btree, index, TUXKEY_LIMIT);
}

int tux3_purge_inode(struct inode *inode, struct tux3_iattr_data *idata,
		     unsigned delta)
{
	int err, has_hole;

	/*
	 * If there is hole extents, i_size was changed and is not
	 * represent last extent in dtree.
	 *
	 * So, clear hole extents, then free all extents.
	 */
	has_hole = tux3_clear_hole(inode, delta);

	/*
	 * FIXME: i_blocks (if implemented) would be better way than
	 * inode->i_size to know whether we have to traverse
	 * btree. (Or another better info?)
	 *
	 * inode->i_size = 0;
	 * if (inode->i_blocks)
	 */
	if (idata->i_size || has_hole) {
		idata->i_size = 0;
		err = tux3_truncate_blocks(inode, 0);
		if (err)
			goto error;
	}
	err = btree_free_empty(&tux_inode(inode)->btree);
	if (err)
		goto error;

	err = xcache_remove_all(inode);
	if (err)
		goto error;

	err = purge_inode(inode);

error:
	return err;
}

/*
 * Decide whether in-core inode can be freed or not.
 */
int tux3_drop_inode(struct inode *inode)
{
	if (!is_bad_inode(inode)) {
		/* If inode->i_nlink == 0, mark dirty to delete */
		if (inode->i_nlink == 0)
			tux3_mark_inode_to_delete(inode);

		/* If inode is dirty, we still keep in-core inode. */
		if (inode->i_state & I_DIRTY)
			return 0;
	}
	return generic_drop_inode(inode);
}

static inline void tux3_evict_inode_check(struct inode *inode)
{
	/* inode should be clean */
	if ((inode->i_state & I_DIRTY) || tux3_check_tuxinode_state(inode)) {
		tux3_err(tux_sb(inode->i_sb),
			 "inode %p, inum %Lu, state %lx/%x",
			 inode, tux_inode(inode)->inum, inode->i_state,
			 tux_inode(inode)->state);
		assert(0);
	}
	assert(!is_defer_alloc_inum(inode));
}

/*
 * In-core inode is going to be freed, do job for it.
 */
void tux3_evict_inode(struct inode *inode)
{
	struct sb *sb = tux_sb(inode->i_sb);
	void *ptr;

	tux3_evict_inode_check(inode);

	/*
	 * evict_inode() should be called only if there is no
	 * in-progress buffers in backend. So we would not need to
	 * care about the page forking here.
	 *
	 * We don't change anything here though, change_{begin,end}
	 * are needed to provide the current delta for debugging in
	 * tux3_invalidate_buffer().
	 *
	 * The ->evict_inode() is called from slab reclaim path, and
	 * reclaim path is called from memory allocation path, so, we
	 * have to use *_nested() here.
	 */
	change_begin_atomic_nested(sb, &ptr);
#ifdef __KERNEL__
	/* Block device special file is still overwriting i_mapping */
	truncate_inode_pages_final(&inode->i_data);
#else
	truncate_inode_pages_final(mapping(inode));
#endif
	change_end_atomic_nested(sb, ptr);


	/*
	 * On theory, reader can be holding the forked page until
	 * evicting inode. So, we have to check the related forked
	 * page, and free forked pages before freeing host
	 * inode. Because page->mapping points freed inode->i_mapping.
	 *
	 * FIXME: we would want to avoid this (e.g. we want to use
	 * refcount to free). If impossible, we would want to use
	 * per-inode forked-buffers list, instead.
	 */
	free_forked_buffers(sb, inode, 1);

	clear_inode(inode);
	free_xcache(inode);
}

static inline void tux_setup_inode_common(struct inode *inode)
{
	struct sb *sb = tux_sb(inode->i_sb);

	assert(tux_inode(inode)->inum != TUX_INVALID_INO);

	switch (inode->i_mode & S_IFMT) {
	case S_IFSOCK:
	case S_IFIFO:
	case S_IFBLK:
	case S_IFCHR:
	case S_IFREG:
		/* Use default gfp type */
		break;
	case S_IFLNK:
	case S_IFDIR:
		inode_nohighmem(inode);
		break;
	case 0: /* internal inode */
	{
		inum_t inum = tux_inode(inode)->inum;
		gfp_t gfp_mask = GFP_USER;

		/* FIXME: bitmap, logmap, vtable, atable doesn't have S_IFMT */
		switch (inum) {
		case TUX_BITMAP_INO:
		case TUX_COUNTMAP_INO:
			/* Accessed from backend only, and flushed on unify */
			tux3_inode_set_flag(TUX3_I_UNIFY, inode);
			fallthrough;
		case TUX_VTABLE_INO:
		case TUX_ATABLE_INO:
			/* set fake i_size to escape the check of block_* */
			inode->i_size = vfs_sb(sb)->s_maxbytes;
			/* Flushed by tux3_flush_inode_internal() */
			tux3_inode_set_flag(TUX3_I_NO_FLUSH, inode);
			break;
		case TUX_VOLMAP_INO:
		case TUX_LOGMAP_INO:
			inode->i_size = (loff_t)sb->volblocks << sb->blockbits;
			/* Flushed by tux3_flush_inode_internal() */
			tux3_inode_set_flag(TUX3_I_NO_FLUSH, inode);
			/* No per-delta buffers, and no page forking */
			tux3_inode_set_flag(TUX3_I_NO_DELTA, inode);
			break;
		default:
			BUG();
			break;
		}

		/*
		 * FIXME: volmap inode is not always dirty. Because
		 * tux3_mark_buffer_unify() doesn't mark tuxnode->flags
		 * as dirty. But, it marks inode->i_state as dirty,
		 * so this is called to prevent to add inode into
		 * dirty list by replay for unify.
		 *
		 * See, FIXME in tux3_mark_buffer_unify().
		 */
		switch (inum) {
		case TUX_BITMAP_INO:
		case TUX_COUNTMAP_INO:
		case TUX_VOLMAP_INO:
			tux3_set_inode_always_dirty(inode);
			break;
		}

		/* Prevent reentering into our fs recursively by mem reclaim */
		switch (inum) {
		case TUX_BITMAP_INO:
		case TUX_COUNTMAP_INO:
		case TUX_VOLMAP_INO:
		case TUX_LOGMAP_INO:
			/* FIXME: we should use non-__GFP_FS for all? */
			gfp_mask &= ~__GFP_FS;

			/* FIXME: can we guarantee forward progress? */
			if (inum == TUX_LOGMAP_INO)
				gfp_mask |= __GFP_NOFAIL;
			break;
		}
		mapping_set_gfp_mask(mapping(inode), gfp_mask);
		break;
	}
	default:
		tux3_fs_error(sb, "Unknown mode: inum %Lx, mode %07ho",
			      tux_inode(inode)->inum, inode->i_mode);
		break;
	}
}

int tux3_setattr(struct user_namespace *mnt_userns,
		 struct dentry *dentry, struct iattr *iattr)
{
	struct inode *inode = d_inode(dentry);
	struct sb *sb = tux_sb(inode->i_sb);
	int err, need_truncate = 0, need_lock = 0;

	err = setattr_prepare(&init_user_ns, dentry, iattr);
	if (err)
		return err;

	if (iattr->ia_valid & ATTR_SIZE && iattr->ia_size != inode->i_size) {
		inode_dio_wait(inode);
		need_truncate = 1;
		/* If truncate pages, this can race with mmap write */
		if (iattr->ia_size < inode->i_size)
			need_lock = 1;
	}

	if (need_lock)
		down_write(&tux_inode(inode)->truncate_lock);
	if (change_begin(sb, 2)) {
		err = -ENOSPC;
		goto unlock;
	}

	if (need_truncate)
		err = tux3_truncate(inode, iattr->ia_size);
	if (!err) {
		tux3_iattrdirty(inode);
		setattr_copy(&init_user_ns, inode, iattr);
		tux3_mark_inode_dirty(inode);
	}

	change_end(sb);
unlock:
	if (need_lock)
		up_write(&tux_inode(inode)->truncate_lock);

	return err;
}

#ifdef __KERNEL__
int tux3_getattr(struct user_namespace *mnt_uerns, const struct path *path,
		 struct kstat *stat, u32 request_mask, unsigned int flags)
{
	struct inode *inode = d_inode(path->dentry);
	struct sb *sb = tux_sb(inode->i_sb);

	generic_fillattr(&init_user_ns, inode, stat);
	stat->ino = tux_inode(inode)->inum;
	/*
	 * FIXME: need to implement ->i_blocks?
	 *
	 * If we want to add i_blocks account, we have to check
	 * existent extent for dirty buffer.  And only if there is no
	 * existent extent, we add to ->i_blocks.
	 *
	 * Yes, ->i_blocks must be including delayed allocation buffers
	 * as allocated, because some apps (e.g. tar) think it is empty file
	 * if i_blocks == 0.
	 *
	 * But, it is purely unnecessary overhead.
	 */
	stat->blocks = ALIGN(i_size_read(inode), sb->blocksize) >> 9;
	return 0;
}

/* This is used by tux3_clear_dirty_inodes() to tell inode state was changed */
void iget_if_dirty(struct inode *inode)
{
	assert(!(inode->i_state & I_FREEING));
	if (atomic_read(&inode->i_count)) {
		atomic_inc(&inode->i_count);
		return;
	}
	/* i_count == 0 should happen only dirty inode */
	assert(inode->i_state & I_DIRTY);
	atomic_inc(&inode->i_count);
}

/* Synchronize changes to a file and directory. */
int tux3_sync_file(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct inode *inode = file->f_mapping->host;
	struct sb *sb = tux_sb(inode->i_sb);

	/* FIXME: this is sync(2). We should implement real one */
	static int print_once;
	if (!print_once) {
		print_once++;
		tux3_warn(sb,
			  "fsync(2) fall-back to sync(2): %Lx-%Lx, datasync %d",
			  start, end, datasync);
	}

	return sync_current_delta(sb);
}

/*
 * Timestamp handler for regular file.
 */
static int tux3_file_update_time(struct inode *inode, struct timespec64 *time,
				 int flags)
{
	/* FIXME: atime is not supported yet */
	if (flags & S_ATIME)
		inode->i_atime = *time;
	if (!(flags & ~S_ATIME))
		return 0;

	tux3_iattrdirty(inode);
	return generic_update_time(inode, time, flags);
}

/*
 * Timestamp handler for directory/symlink. Timestamp is updated by
 * directly, so this should not be called except atime.
 */
int tux3_no_update_time(struct inode *inode, struct timespec64 *time, int flags)
{
	/* FIXME: atime is not supported yet */
	if (flags & S_ATIME)
		inode->i_atime = *time;
	if (!(flags & ~S_ATIME))
		return 0;

	WARN(1, "update_time is called for inum %Lu, i_mode %x\n",
	     tux_inode(inode)->inum, inode->i_mode);

	return 0;
}

/*
 * Timestamp handler for special file.  This is not called under
 * change_{begin,end}() or inode_lock.
 *
 * FIXME: special file should also handle timestamp as transaction?
 */
static int tux3_special_update_time(struct inode *inode,
				    struct timespec64 *time, int flags)
{
	struct sb *sb = tux_sb(inode->i_sb);
	int err;

	/* FIXME: atime is not supported yet */
	if (flags & S_ATIME)
		inode->i_atime = *time;
	if (!(flags & ~S_ATIME))
		return 0;

	/* FIXME: no inode_lock, so this is racy */
	if (change_begin(sb, 1))
		return -ENOSPC;
	err = generic_update_time(inode, time, flags);
	change_end(sb);

	return err;
}

#include "inode_vfslib.c"

static const struct file_operations tux_file_fops = {
	.llseek		= generic_file_llseek,
	.read_iter	= generic_file_read_iter,
	.write_iter	= tux3_file_write_iter,
//	.unlocked_ioctl	= fat_generic_ioctl,
#ifdef CONFIG_COMPAT
//	.compat_ioctl	= fat_compat_dir_ioctl,
#endif
	.mmap		= tux3_file_mmap,
	.open		= generic_file_open,
	.fsync		= tux3_sync_file,
	.splice_read	= generic_file_splice_read,
	.splice_write	= iter_file_splice_write,
};

static const struct inode_operations tux_file_iops = {
//	.permission	= ext4_permission,
	.setattr	= tux3_setattr,
	.getattr	= tux3_getattr,
#ifdef CONFIG_EXT4DEV_FS_XATTR
//	.setxattr	= generic_setxattr,
//	.getxattr	= generic_getxattr,
//	.listxattr	= ext4_listxattr,
//	.removexattr	= generic_removexattr,
#endif
//	.fallocate	= ext4_fallocate,
//	.fiemap		= ext4_fiemap,
	.update_time	= tux3_file_update_time,
};

static const struct inode_operations tux_special_iops = {
//	.permission	= ext4_permission,
	.setattr	= tux3_setattr,
	.getattr	= tux3_getattr,
#ifdef CONFIG_EXT4DEV_FS_XATTR
//	.setxattr	= generic_setxattr,
//	.getxattr	= generic_getxattr,
//	.listxattr	= ext4_listxattr,
//	.removexattr	= generic_removexattr,
#endif
	.update_time	= tux3_special_update_time,
};

const struct inode_operations tux_symlink_iops = {
	.get_link	= page_get_link,
	.setattr	= tux3_setattr,
	.getattr	= tux3_getattr,
#if 0
//	.setxattr	= generic_setxattr,
//	.getxattr	= generic_getxattr,
//	.listxattr	= ext4_listxattr,
//	.removexattr	= generic_removexattr,
#endif
	.update_time	= tux3_no_update_time,
};

static void tux_setup_inode(struct inode *inode)
{
	tux_setup_inode_common(inode);

//	inode->i_generation = 0;
//	inode->i_flags = 0;

	switch (inode->i_mode & S_IFMT) {
	case S_IFSOCK:
	case S_IFIFO:
	case S_IFBLK:
	case S_IFCHR:
		inode->i_op = &tux_special_iops;
		init_special_inode(inode, inode->i_mode, inode->i_rdev);
		break;
	case S_IFREG:
		inode->i_op = &tux_file_iops;
		inode->i_fop = &tux_file_fops;
		inode->i_mapping->a_ops = &tux_file_aops;
//		tux_inode(inode)->io = tux3_filemap_overwrite_io;
		tux_inode(inode)->io = tux3_filemap_redirect_io;
		break;
	case S_IFDIR:
		inode->i_op = &tux_dir_iops;
		inode->i_fop = &tux_dir_fops;
		inode->i_mapping->a_ops = &tux_blk_aops;
		tux_inode(inode)->io = tux3_filemap_redirect_io;
		break;
	case S_IFLNK:
		inode->i_op = &tux_symlink_iops;
		inode->i_mapping->a_ops = &tux_symlink_aops;
		tux_inode(inode)->io = tux3_filemap_redirect_io;
		break;
	case 0: /* internal inode */
		/* FIXME: bitmap, logmap, vtable, atable doesn't have S_IFMT */
		switch (tux_inode(inode)->inum) {
		case TUX_BITMAP_INO:
		case TUX_COUNTMAP_INO:
		case TUX_VTABLE_INO:
		case TUX_ATABLE_INO:
			inode->i_mapping->a_ops = &tux_blk_aops;
			tux_inode(inode)->io = tux3_filemap_redirect_io;
			break;
		case TUX_VOLMAP_INO:
		case TUX_LOGMAP_INO:
			inode->i_mapping->a_ops = &tux_vol_aops;
			if (tux_inode(inode)->inum == TUX_VOLMAP_INO)
				tux_inode(inode)->io = tux3_volmap_early_io;
			else
				tux_inode(inode)->io = tux3_logmap_io;
			break;
		}
		break;
	}
}
#endif /* __KERNEL__ */
