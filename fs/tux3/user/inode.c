/*
 * Tux3 versioning filesystem in user space
 *
 * Original copyright (c) 2008 Daniel Phillips <phillips@phunq.net>
 * Licensed under the GPL version 3
 *
 * By contributing changes to this file you grant the original copyright holder
 * the right to distribute those changes under any license.
 */

#include "tux3user.h"

#ifndef trace
#define trace trace_on
#endif

#define HASH_SHIFT	10
#define HASH_SIZE	(1 << 10)
#define HASH_MASK	(HASH_SIZE - 1)

static struct hlist_head inode_hashtable[HASH_SIZE] = {
	[0 ... (HASH_SIZE - 1)] = HLIST_HEAD_INIT,
};

static unsigned int hash(inum_t inum)
{
	return hash_64(inum, HASH_SHIFT);
}

void inode_leak_check(void)
{
	int leaks = 0;

	for (int i = 0; i < HASH_SIZE; i++) {
		struct hlist_head *head = inode_hashtable + i;
		struct inode *inode;
		hlist_for_each_entry(inode, head, i_hash) {
			trace_on("possible leak inode inum %Lu, i_count %d",
				 tux_inode(inode)->inum,
				 atomic_read(&inode->i_count));
			leaks++;
		}
	}

	assert(leaks == 0);
}

static inline int inode_unhashed(struct inode *inode)
{
	return hlist_unhashed(&inode->i_hash);
}

static void insert_inode_hash(struct inode *inode)
{
	struct hlist_head *b = inode_hashtable + hash(tux_inode(inode)->inum);
	hlist_add_head(&inode->i_hash, b);
}

void remove_inode_hash(struct inode *inode)
{
	if (!inode_unhashed(inode))
		hlist_del_init(&inode->i_hash);
}

void inode_init_once(struct inode *inode)
{
	memset(inode, 0, sizeof(*inode));
	INIT_HLIST_NODE(&inode->i_hash);
}

static void inode_init_always(struct super_block *sb, struct inode *inode)
{
	inode->i_sb	= sb;
	inode->i_nlink	= 1;
	atomic_set(&inode->i_count, 1);

	spin_lock_init(&inode->i_lock);
	init_rwsem(&inode->i_rwsem);
	mapping(inode)->inode = inode;
	mapping_set_gfp_mask(mapping(inode), GFP_HIGHUSER_MOVABLE);
}

static void destroy_inode(struct inode *inode)
{
	assert(hlist_unhashed(&inode->i_hash));

	if (mapping(inode))
		free_map(mapping(inode));
	__destroy_inode(inode);
}

static struct inode *alloc_inode(struct super_block *sb)
{
	struct inode *inode = __alloc_inode(sb);
	if (!inode)
		goto error;

	inode->map = new_map(sb_dev(tux_sb(sb)), NULL);
	if (!inode->map)
		goto error_map;

	inode_init_always(sb, inode);

	return inode;

error_map:
	destroy_inode(inode);
error:
	return NULL;
}

static struct inode *new_inode(struct super_block *sb)
{
	struct inode *inode = alloc_inode(sb);

	if (inode) {
		spin_lock(&inode->i_lock);
		inode->i_state = 0;
		spin_unlock(&inode->i_lock);
	}
	return inode;
}

/* This is just to clean inode is partially initialized */
static void make_bad_inode(struct inode *inode)
{
	remove_inode_hash(inode);
	inode->i_state |= I_BAD;
}

static int is_bad_inode(struct inode *inode)
{
	return inode->i_state & I_BAD;
}

void unlock_new_inode(struct inode *inode)
{
	spin_lock(&inode->i_lock);
	WARN_ON(!(inode->i_state & I_NEW));
	inode->i_state &= ~I_NEW & ~I_CREATING;
	spin_unlock(&inode->i_lock);
}

void discard_new_inode(struct inode *inode)
{
	spin_lock(&inode->i_lock);
	WARN_ON(!(inode->i_state & I_NEW));
	inode->i_state &= ~I_NEW;
	spin_unlock(&inode->i_lock);
	iput(inode);
}

static void iget_failed(struct inode *inode)
{
	make_bad_inode(inode);
	unlock_new_inode(inode);
	iput(inode);
}

void __iget(struct inode *inode)
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

/* This is used by tux3_clear_dirty_inodes() to tell inode state was changed */
void iget_if_dirty(struct inode *inode)
{
	__iget(inode);
}

/* get additional reference to inode; caller must already hold one. */
void ihold(struct inode *inode)
{
	assert(!(inode->i_state & I_FREEING));
	assert(atomic_read(&inode->i_count) >= 1);
	atomic_inc(&inode->i_count);
}

static struct inode *find_inode(struct super_block *sb, struct hlist_head *head,
				int (*test)(struct inode *, void *),
				void *data)
{
	struct inode *inode = NULL;

repeat:
	hlist_for_each_entry(inode, head, i_hash) {
		if (inode->i_sb != sb)
			continue;
		if (!test(inode, data))
			continue;
		spin_lock(&inode->i_lock);
		if (inode->i_state & (I_FREEING|I_WILL_FREE)) {
			assert(0);	/* On userland, shouldn't happen */
			spin_unlock(&inode->i_lock);
			goto repeat;
		}
		if (unlikely(inode->i_state & I_CREATING)) {
			spin_unlock(&inode->i_lock);
			return ERR_PTR(-ESTALE);
		}
		__iget(inode);
		spin_unlock(&inode->i_lock);
		return inode;
	}
	return NULL;
}

static struct inode *ilookup5_nowait(struct super_block *sb, inum_t inum,
		int (*test)(struct inode *, void *), void *data)
{
	struct hlist_head *head = inode_hashtable + hash(inum);
	struct inode *inode;

	inode = find_inode(sb, head, test, data);

	return inode;
}

static struct inode *inode_insert5(struct inode *inode, inum_t inum,
			    int (*test)(struct inode *, void *),
			    int (*set)(struct inode *, void *), void *data)
{
	struct hlist_head *head = inode_hashtable + hash(inum);
	struct inode *old;

again:
	old = find_inode(inode->i_sb, head, test, data);
	if (unlikely(old)) {
		/*
		 * Uhhuh, somebody else created the same inode under us.
		 * Use the old inode instead of the preallocated one.
		 */
		if (IS_ERR(old))
			return NULL;
		assert(!(inode->i_state & I_NEW));
		//wait_on_inode(old);
		if (unlikely(inode_unhashed(old))) {
			iput(old);
			goto again;
		}
		return old;
	}

	if (set && unlikely(set(inode, data))) {
		inode = NULL;
		goto unlock;
	}

	/*
	 * Return the locked inode with I_NEW set, the
	 * caller is responsible for filling in the contents
	 */
	spin_lock(&inode->i_lock);
	inode->i_state |= I_NEW;
	hlist_add_head(&inode->i_hash, head);
	spin_unlock(&inode->i_lock);
unlock:

	return inode;
}

static struct inode *ilookup5(struct super_block *sb, inum_t inum,
		int (*test)(struct inode *, void *), void *data)
{
	struct inode *inode;
again:
	inode = ilookup5_nowait(sb, inum, test, data);
	if (inode) {
		/* On userland, inode shouldn't have I_NEW */
		assert(!(inode->i_state & I_NEW));
		//wait_on_inode(inode);
		if (unlikely(inode_unhashed(inode))) {
			iput(inode);
			goto again;
		}
	}
	return inode;
}

static struct inode *iget5_locked(struct super_block *sb, inum_t inum,
			   int (*test)(struct inode *, void *),
			   int (*set)(struct inode *, void *), void *data)
{
	struct inode *inode = ilookup5(sb, inum, test, data);

	if (!inode) {
		struct inode *new = alloc_inode(sb);

		if (new) {
			new->i_state = 0;
			inode = inode_insert5(new, inum, test, set, data);
			if (unlikely(inode != new))
				destroy_inode(new);
		}
	}
	return inode;
}

static int insert_inode_locked4(struct inode *inode, inum_t inum,
			 int (*test)(struct inode *, void *), void *data)
{
	struct inode *old;

	inode->i_state |= I_CREATING;
	old = inode_insert5(inode, inum, test, NULL, data);

	if (old != inode) {
		iput(old);
		return -EBUSY;
	}
	return 0;
}

loff_t i_size_read(const struct inode *inode)
{
	return inode->i_size;
}

void i_size_write(struct inode *inode, loff_t i_size)
{
	inode->i_size = i_size;
}

/* For now, we doesn't cache inode */
static int generic_drop_inode(struct inode *inode)
{
	return 1;
}

#include "../inode.c"

static void tux_setup_inode(struct inode *inode)
{
	tux_setup_inode_common(inode);

	switch (inode->i_mode & S_IFMT) {
	case S_IFSOCK:
	case S_IFIFO:
	case S_IFBLK:
	case S_IFCHR:
		inode->map->io = dev_errio;
		break;
	case S_IFREG:
//		inode->map->io = tux3_filemap_overwrite_io;
		inode->map->io = tux3_filemap_redirect_io;
		break;
	case S_IFDIR:
	case S_IFLNK:
		inode->map->io = tux3_filemap_redirect_io;
		break;
	case 0: /* internal inode */
		/* FIXME: bitmap, logmap, vtable, atable doesn't have S_IFMT */
		switch (tux_inode(inode)->inum) {
		case TUX_BITMAP_INO:
		case TUX_COUNTMAP_INO:
		case TUX_VTABLE_INO:
		case TUX_ATABLE_INO:
			inode->map->io = tux3_filemap_redirect_io;
			break;
		case TUX_VOLMAP_INO:
		case TUX_LOGMAP_INO:
			if (tux_inode(inode)->inum == TUX_VOLMAP_INO)
				/* use default handler (dev_blockio) */;
			else
				inode->map->io = tux3_logmap_io;
			break;
		}
		break;
	}
}

static void iput_final(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	unsigned long state;
	int drop;

	WARN_ON(inode->i_state & I_NEW);

	drop = tux3_drop_inode(inode);
	if (!drop && (sb->s_flags & SB_ACTIVE)) {
		/* Keep the inode on dirty list */
		spin_unlock(&inode->i_lock);
		return;
	}

	state = inode->i_state;
	if (!drop) {
		WRITE_ONCE(inode->i_state, state | I_WILL_FREE);
		spin_unlock(&inode->i_lock);

		BUG_ON(1);
		//write_inode_now(inode, 1);

		spin_lock(&inode->i_lock);
		state = inode->i_state;
		WARN_ON(state & I_NEW);
		state &= ~I_WILL_FREE;
	}

	WRITE_ONCE(inode->i_state, state | I_FREEING);
	spin_unlock(&inode->i_lock);

	tux3_evict_inode(inode);

	remove_inode_hash(inode);
	destroy_inode(inode);
}

/*
 * NOTE: iput() must not be called inside of change_begin/end() if
 * i_nlink == 0.  Otherwise, it will become cause of deadlock.
 */
void iput(struct inode *inode)
{
	if (inode == NULL)
		return;

	if (atomic_dec_and_lock(&inode->i_count, &inode->i_lock)) {
		iput_final(inode);
	}
}

/* For unit test */
int __tuxtruncate(struct inode *inode, loff_t size)
{
	return tux3_truncate(inode, size);
}

int tuxtruncate(struct inode *inode, loff_t size)
{
	struct dentry dentry = {
		.d_inode = inode,
	};
	struct iattr iattrs = {
		.ia_valid = ATTR_SIZE,
		.ia_size = size,
	};

	if (size < 0)
		return -EINVAL;

	return tux3_setattr(&init_user_ns, &dentry, &iattrs);
}

/* Easy way to make a dummy inode. */
struct inode *rapid_new_inode(struct sb *sb, blockio_t *io, umode_t mode)
{
	struct tux_iattr iattr = {
		.mode = mode,
	};
	struct inode *inode;

	inode = tux_new_inode(sb, NULL, &iattr);
	assert(inode);

	inode->map->io = io;

	return inode;
}

void rapid_free_inode(struct inode *inode)
{
	/*
	 * The test code may leave dirty state on rapid inode, so
	 * clear it here to make happy sanity check.
	 */
	if (inode->i_state & I_DIRTY) {
		/*
		 * Assuming the user of rapid inode never do delta
		 * transition. So use TUX3_INIT_DELTA always.
		 */
		__tux3_clear_dirty_inode(inode, TUX3_INIT_DELTA);
		printf("Note: rapid inode (inum %llu) was dirtied, it may not work.\n",
		       tux_inode(inode)->inum);
	}

	destroy_inode(inode);
}
