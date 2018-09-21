/*
 * Generic btree operations.
 *
 * Copyright (c) 2008-2014 Daniel Phillips
 * Copyright (c) 2008-2014 OGAWA Hirofumi
 */

#include "tux3.h"

#ifndef trace
#define trace trace_off
#endif

static inline int bnode_key_compare(const void *__p1, const void *__p2)
{
	u64 p1 = be64_to_cpup(__p1);
	u64 p2 = be64_to_cpup(__p2);

	if (p1 < p2)
		return -1;
	else if (p1 > p2)
		return 1;
	return 0;
}

/* The bnode format. See bdict_fixed.c. */
#define BNODE_KEY_SIZE		sizeof(__be64)
#define BNODE_DATA_SIZE		sizeof(__be64)

#define FBDICT_BIG_ENDIAN	true
#define FBDICT_KEY_SIZE		BNODE_KEY_SIZE
#define FBDICT_DATA_SIZE	BNODE_DATA_SIZE
#define FBDICT_COMPARE(k, p)	bnode_key_compare(k, p)
#define FBDICT_ZERO_CLEAR	true
#define FBDICT_NEED_SPLIT	true
#define FBDICT_NEED_MERGE	true
#include "bdict_fixed.c"

/* This value is special case to tell btree doesn't have root yet. */
struct root no_root = {
	.block	= 0,
	.depth	= 0,
};

struct bnode {
	__be16 magic;
	__be16 unused1;
	__be16 unused2;
	u8 fbdict[];
};

/*
 * Note that the first key of an index block is never accessed.  This is
 * because for a btree, there is always one more key than bnodes in each
 * index bnode.  In other words, keys lie between bnode pointers.  I
 * micro-optimize by placing the bnode count in the first key, which allows
 * a bnode to contain an esthetically pleasing binary number of pointers.
 * (Not done yet.)
 */

void btree_init_param(struct sb *sb)
{
	sb->bnode_dict_size = sb->blocksize - sizeof(struct bnode);
	sb->bnode_max_count = fbdict_max_count(sb->bnode_dict_size);
}

static inline unsigned bcount(struct bnode *bnode)
{
	return fbdict_count(bnode->fbdict);
}

static inline int bnode_used_size(struct bnode *bnode)
{
	return sizeof(bnode) + fbdict_used_size(bnode->fbdict);
}

static inline void bnode_buffer_init(struct sb *sb, struct buffer_head *buffer)
{
	struct bnode *bnode = bufdata(buffer);
	memset(bnode, 0, sizeof(*bnode));
	bnode->magic = cpu_to_be16(TUX3_MAGIC_BNODE);
	fbdict_init(bnode->fbdict, sb->bnode_dict_size);
}

static inline int bnode_sniff(struct bnode *bnode)
{
	if (bnode->magic != cpu_to_be16(TUX3_MAGIC_BNODE))
		return -1;
	return 0;
}

static inline __be64 *bnode_keyp(struct bnode *bnode, int idx)
{
	return fbdict_keyp(bnode->fbdict, idx);
}

static inline __be64 *bnode_blockp(struct sb *sb, struct bnode *bnode, int idx)
{
	return fbdict_datap(bnode->fbdict, sb->bnode_dict_size, idx);
}

/* Lookup the index entry contains key */
static int bnode_lookup(struct bnode *bnode, tuxkey_t key)
{
	__be64 __key = cpu_to_be64(key);
	assert(bcount(bnode) > 0);
	/* Most left key can be invalid, so exclude. */
	int idx = fbdict_lookup(bnode->fbdict, &__key, 1);
	if (idx == -1)
		idx = 0;	/* bcount=0 or all are larger than key */
	if (0) {
		/* Paranoia check idx with linear search */
		int next = 0, count = bcount(bnode);
		assert(count > 0);
		while (++next < count) {
			if (be64_to_cpup(bnode_keyp(bnode, next)) > key)
				break;
		}
		BUG_ON(idx != next - 1);
	}
	return idx;
}

static struct buffer_head *new_block(struct btree *btree)
{
	block_t block;

	block = balloc_one(btree->sb);
	if (block < 0)
		return ERR_PTR(block);
	struct buffer_head *buffer = vol_getblk(btree->sb, block);
	if (!buffer)
		return ERR_PTR(-ENOMEM); // ERR_PTR me!!! and bfree?
	return buffer;
}

struct buffer_head *new_leaf(struct btree *btree)
{
	struct buffer_head *buffer = new_block(btree);

	if (!IS_ERR(buffer)) {
		memset(bufdata(buffer), 0, bufsize(buffer));
		btree->ops->leaf_init(btree, bufdata(buffer));
		mark_buffer_dirty_atomic(buffer);
	}
	return buffer;
}

static struct buffer_head *new_bnode(struct btree *btree)
{
	struct buffer_head *buffer = new_block(btree);

	if (!IS_ERR(buffer)) {
		bnode_buffer_init(btree->sb, buffer);
		mark_buffer_unify_atomic(buffer);
	}
	return buffer;
}

/*
 * A btree cursor has n entries for a btree of depth n, with the first n - 1
 * entries pointing at internal nodes and entry n pointing at a leaf.
 * The next field points at the next index entry that will be loaded in a left
 * to right tree traversal, not the current entry.  The next pointer is null
 * for the leaf, which has its own specialized traversal algorithms.
 */

static inline struct bnode *level_bnode(struct path_level *at)
{
	return bufdata(at->buffer);
}

static inline __be64 *level_this_keyp(struct path_level *at)
{
	return bnode_keyp(level_bnode(at), at->next - 1);
}

static inline __be64 *level_next_keyp(struct path_level *at)
{
	return bnode_keyp(level_bnode(at), at->next);
}

static inline __be64 *level_this_blockp(struct sb *sb, struct path_level *at)
{
	return bnode_blockp(sb, level_bnode(at), at->next - 1);
}

static inline __be64 *level_next_blockp(struct sb *sb, struct path_level *at)
{
	return bnode_blockp(sb, level_bnode(at), at->next);
}

/* There is no next entry? */
static inline bool level_finished(struct path_level *at)
{
	return at->next == bcount(level_bnode(at));
}
// also write level_beginning!!!

static void level_replace_blockput(struct path_level *at,
				   struct buffer_head *buffer, int next)
{
#ifdef CURSOR_DEBUG
	assert(buffer);
	assert(at->buffer != FREE_BUFFER);
	assert(at->next != FREE_NEXT);
#endif
	blockput(at->buffer);
	at->buffer = buffer;
	at->next = next;
}

static void level_redirect_blockput(struct path_level *at,
				    struct buffer_head *clone)
{
	memcpy(bufdata(clone), bufdata(at->buffer), bufsize(clone));
	level_replace_blockput(at, clone, at->next);
}

#ifdef CURSOR_DEBUG
static void cursor_check(struct cursor *cursor)
{
	struct sb *sb = cursor->btree->sb;
	block_t block = cursor->btree->root.block;
	tuxkey_t key = 0;
	int i;

	for (i = 0; i <= cursor->level; i++) {
		struct path_level *at = &cursor->path[i];

		assert(bufindex(at->buffer) == block);
		if (i == cursor->level)
			break;

		struct bnode *bnode = level_bnode(at);
		int idx = at->next - 1;
		assert(0 <= idx);
		assert(idx < bcount(bnode));
		/*
		 * If this entry is most left, it should be same key
		 * with parent. Otherwise, most left key may not be
		 * correct as next key.
		 */
		if (idx == 0)
			assert(be64_to_cpup(bnode_keyp(bnode, idx)) == key);
		else
			assert(be64_to_cpup(bnode_keyp(bnode, idx)) > key);

		key = be64_to_cpup(bnode_keyp(bnode, idx));
		block = be64_to_cpup(bnode_blockp(sb, bnode, idx));
	}
}
#else
static inline void cursor_check(struct cursor *cursor) {}
#endif

struct buffer_head *cursor_leafbuf(struct cursor *cursor)
{
	assert(cursor->level == cursor->btree->root.depth - 1);
	return cursor->path[cursor->level].buffer;
}

static void cursor_root_add(struct cursor *cursor, struct buffer_head *buffer,
			    int next)
{
#ifdef CURSOR_DEBUG
	assert(cursor->level < cursor->maxlevel);
	assert(cursor->path[cursor->level + 1].buffer == FREE_BUFFER);
	assert(cursor->path[cursor->level + 1].next == FREE_NEXT);
#endif
	vecmove(cursor->path + 1, cursor->path, cursor->level + 1);
	cursor->level++;
	cursor->path[0].buffer = buffer;
	cursor->path[0].next = next;
}

static void cursor_push(struct cursor *cursor, struct buffer_head *buffer,
			int next)
{
	cursor->level++;
#ifdef CURSOR_DEBUG
	assert(cursor->level <= cursor->maxlevel);
	assert(cursor->path[cursor->level].buffer == FREE_BUFFER);
	assert(cursor->path[cursor->level].next == FREE_NEXT);
#endif
	cursor->path[cursor->level].buffer = buffer;
	cursor->path[cursor->level].next = next;
}

static int cursor_push_one(struct cursor *cursor, struct buffer_head *buffer)
{
	struct btree *btree = cursor->btree;
	int ret, next;

	assert(btree->root.depth >= 1);

	/* Is this the bnode level? */
	if (cursor->level < btree->root.depth - 2) {
		struct bnode *bnode = bufdata(buffer);
		assert(!bnode_sniff(bnode));
		next = 0;
		ret = 1;
	} else {
		assert(!btree->ops->leaf_sniff(btree, bufdata(buffer)));
		next = CURSOR_LEAF_LEVEL;
		ret = 0;
	}
	cursor_push(cursor, buffer, next);
	cursor_check(cursor);

	return ret;
}

static struct buffer_head *cursor_pop(struct cursor *cursor)
{
	struct buffer_head *buffer;

#ifdef CURSOR_DEBUG
	assert(cursor->level >= 0);
#endif
	buffer = cursor->path[cursor->level].buffer;
#ifdef CURSOR_DEBUG
	cursor->path[cursor->level].buffer = FREE_BUFFER;
	cursor->path[cursor->level].next = FREE_NEXT;
#endif
	cursor->level--;
	return buffer;
}

static inline void cursor_pop_blockput(struct cursor *cursor)
{
	blockput(cursor_pop(cursor));
}

void release_cursor(struct cursor *cursor)
{
	while (cursor->level >= 0)
		cursor_pop_blockput(cursor);
}

/* unused */
void show_cursor(struct cursor *cursor, int depth)
{
	__tux3_dbg(">>> cursor %p/%i:", cursor, depth);
	int i;
	for (i = 0; i < depth; i++) {
		__tux3_dbg(" [%Lx/%i]",
			   bufindex(cursor->path[i].buffer),
			   bufcount(cursor->path[i].buffer));
	}
	__tux3_dbg("\n");
}

static inline int alloc_cursor_size(int count)
{
	return sizeof(struct cursor) + sizeof(struct path_level) * count;
}

struct cursor *alloc_cursor(struct btree *btree, int extra)
{
	int extra_depth = btree->root.depth + extra;
	struct cursor *cursor;

	cursor = kmalloc(alloc_cursor_size(extra_depth), GFP_NOFS);
	if (cursor) {
		cursor->btree = btree;
		cursor->level = -1;
#ifdef CURSOR_DEBUG
		cursor->maxlevel = extra_depth - 1;
		int i;
		for (i = 0; i < extra_depth; i++) {
			cursor->path[i].buffer = FREE_BUFFER; /* for debug */
			cursor->path[i].next = FREE_NEXT; /* for debug */
		}
#endif
	}
	return cursor;
}

void free_cursor(struct cursor *cursor)
{
#ifdef CURSOR_DEBUG
	if (cursor)
		assert(cursor->level == -1);
#endif
	kfree(cursor);
}

static int cursor_level_finished(struct cursor *cursor)
{
	/* must not be leaf */
	assert(cursor->level < cursor->btree->root.depth - 1);
	return level_finished(&cursor->path[cursor->level]);
}

/*
 * Climb up the cursor until we find the first level where we have not yet read
 * all the way to the end of the index block, there we find the key that
 * separates the subtree we are in (a leaf) from the next subtree to the right.
 */
tuxkey_t cursor_next_key(struct cursor *cursor)
{
	int level = cursor->level;
	assert(level == cursor->btree->root.depth - 1);
	while (level--) {
		struct path_level *at = &cursor->path[level];
		if (!level_finished(at))
			return be64_to_cpup(level_next_keyp(at));
	}
	return TUXKEY_LIMIT;
}

static tuxkey_t cursor_level_next_key(struct cursor *cursor)
{
	int level = cursor->level;
	assert(level < cursor->btree->root.depth - 1);
	while (level >= 0) {
		struct path_level *at = &cursor->path[level];
		if (!level_finished(at))
			return be64_to_cpup(level_next_keyp(at));
		level--;
	}
	return TUXKEY_LIMIT;
}

/* Return key of this leaf */
tuxkey_t cursor_this_key(struct cursor *cursor)
{
	assert(cursor->level == cursor->btree->root.depth - 1);
	if (cursor->btree->root.depth == 1)
		return 0;
	return be64_to_cpup(level_this_keyp(&cursor->path[cursor->level - 1]));
}

static tuxkey_t cursor_level_this_key(struct cursor *cursor)
{
	assert(cursor->level < cursor->btree->root.depth - 1);
	if (cursor->level < 0)
		return 0;
	return be64_to_cpup(level_this_keyp(&cursor->path[cursor->level]));
}

/*
 * Cursor read root bnode/leaf.
 * < 0 - error
 *   0 - there is no further child (leaf was pushed)
 *   1 - there is child
 */
static int cursor_read_root(struct cursor *cursor)
{
	struct btree *btree = cursor->btree;
	struct buffer_head *buffer;

	assert(has_root(btree));

	buffer = vol_bread(btree->sb, btree->root.block);
	if (!buffer)
		return -EIO; /* FIXME: stupid, it might have been NOMEM */

	return cursor_push_one(cursor, buffer);
}

/*
 * Cursor up to parent bnode.
 * 0 - there is no further parent (root was popped)
 * 1 - there is parent
 */
static int cursor_advance_up(struct cursor *cursor)
{
	assert(cursor->level >= 0);
	cursor_pop_blockput(cursor);
	return cursor->level >= 0;
}

/*
 * Cursor down to child bnode or leaf, and update ->next.
 * < 0 - error
 *   0 - there is no further child (leaf was pushed)
 *   1 - there is child
 */
static int cursor_advance_down(struct cursor *cursor)
{
	struct btree *btree = cursor->btree;
	struct path_level *at = &cursor->path[cursor->level];
	struct buffer_head *buffer;
	block_t child;

	assert(cursor->level < btree->root.depth - 1);

	child = be64_to_cpup(level_next_blockp(btree->sb, at));
	buffer = vol_bread(btree->sb, child);
	if (!buffer)
		return -EIO; /* FIXME: stupid, it might have been NOMEM */
	at->next++;

	return cursor_push_one(cursor, buffer);
}

/*
 * Cursor advance for btree traverse.
 * < 0 - error
 *   0 - Finished traverse
 *   1 - Reached leaf
 */
static int cursor_advance(struct cursor *cursor)
{
	int ret;

	do {
		if (!cursor_advance_up(cursor))
			return 0;
	} while (cursor_level_finished(cursor));
	do {
		ret = cursor_advance_down(cursor);
		if (ret < 0)
			return ret;
	} while (ret);

	return 1;
}

/* Lookup index and set it as next down path */
static void cursor_bnode_lookup(struct cursor *cursor, tuxkey_t key)
{
	struct path_level *at = &cursor->path[cursor->level];
	at->next = bnode_lookup(level_bnode(at), key);
}

int btree_probe(struct cursor *cursor, tuxkey_t key)
{
	int ret;

	ret = cursor_read_root(cursor);
	if (ret < 0)
		return ret;

	while (ret) {
		cursor_bnode_lookup(cursor, key);

		ret = cursor_advance_down(cursor);
		if (ret < 0)
			goto error;
	}

	return 0;

error:
	release_cursor(cursor);
	return ret;
}

/*
 * Traverse btree for specified range
 * key: start to traverse (cursor should point leaf is including key)
 * len: length to traverse
 *
 * return value:
 * < 0 - error
 *   0 - traversed all range
 * 0 < - traverse was stopped by func, and return value of func
 */
int btree_traverse(struct cursor *cursor, tuxkey_t key, u64 len,
		   btree_traverse_func_t func, void *data)
{
	struct btree *btree = cursor->btree;
	int ret;

	do {
		tuxkey_t bottom = cursor_this_key(cursor);
		tuxkey_t limit = cursor_next_key(cursor);
		void *leaf = bufdata(cursor_leafbuf(cursor));
		assert(!btree->ops->leaf_sniff(btree, leaf));

		if (key < bottom) {
			len -= min_t(u64, len, bottom - key);
			if (len == 0)
				break;
			key = bottom;
		}

		ret = func(btree, bottom, limit, leaf, key, len, data);
		/* Stop traverse if ret >= 1, or error */
		if (ret)
			goto out;

		/* If next key is out of range, done */
		if (key + len <= limit)
			break;

		ret = cursor_advance(cursor);
		if (ret < 0)
			goto out;
	} while (ret);

	ret = 0;
out:
	return ret;
}

static bool leaf_need_redirect(struct sb *sb, struct buffer_head *buffer)
{
	/* FIXME: leaf doesn't have delta number, we might want to
	 * remove exception for leaf */
	/* If this is not re-dirty, we need to redirect */
	return !buffer_dirty(buffer);
}

static bool bnode_need_redirect(struct sb *sb, struct buffer_head *buffer)
{
	/* If this is not re-dirty for sb->unify, we need to redirect */
	return !buffer_already_dirty(buffer, sb->unify);
}

/*
 * Recursively redirect non-dirty buffers on path to modify leaf.
 *
 * Redirect order is from root to leaf. Otherwise, blocks of path will
 * be allocated by reverse order.
 *
 * FIXME: We can allocate/copy blocks before change common ancestor
 * (before changing common ancestor, changes are not visible for
 * reader). With this, we may be able to reduce locking time.
 */
int cursor_redirect(struct cursor *cursor)
{
	struct btree *btree = cursor->btree;
	struct sb *sb = btree->sb;
	int level;

	for (level = 0; level < btree->root.depth; level++) {
		struct path_level *parent_at, *at = &cursor->path[level];
		struct buffer_head *clone;
		block_t oldblock, newblock;
		bool redirect, is_leaf = (level == btree->root.depth - 1);

		/* If buffer needs to redirect to dirty, redirect it */
		if (is_leaf)
			redirect = leaf_need_redirect(sb, at->buffer);
		else
			redirect = bnode_need_redirect(sb, at->buffer);

		/* No need to redirect */
		if (!redirect)
			continue;

		/* Redirect buffer before changing */
		clone = new_block(btree);
		if (IS_ERR(clone))
			return PTR_ERR(clone);
		oldblock = bufindex(at->buffer);
		newblock = bufindex(clone);
		trace("redirect %Lx to %Lx", oldblock, newblock);
		level_redirect_blockput(at, clone);
		if (is_leaf) {
			/* This is leaf buffer */
			mark_buffer_dirty_atomic(clone);
			log_leaf_redirect(sb, oldblock, newblock);
			defer_bfree(sb, &sb->defree, oldblock, 1);
		} else {
			/* This is bnode buffer */
			mark_buffer_unify_atomic(clone);
			log_bnode_redirect(sb, oldblock, newblock);
			defer_bfree(sb, &sb->deunify, oldblock, 1);
		}

		trace("update parent");
		if (!level) {
			/* Update pointer in btree->root */
			trace("redirect root");
			assert(oldblock == btree->root.block);
			btree->root.block = newblock;
			tux3_mark_btree_dirty(btree);
			continue;
		}
		/* Update entry on parent for the redirected block */
		parent_at = at - 1;
		*level_this_blockp(sb, parent_at) = cpu_to_be64(newblock);
		log_bnode_update(sb, bufindex(parent_at->buffer), newblock,
				 be64_to_cpup(level_this_keyp(parent_at)));
	}

	cursor_check(cursor);
	return 0;
}

/* Deletion */

static void bnode_remove_index(struct sb *sb, struct bnode *bnode,
			       int idx, int nr)
{
	fbdict_delete(bnode->fbdict, sb->bnode_dict_size, idx, nr);
}

static int bnode_merge_bnodes(struct sb *sb, struct bnode *into,
			      struct bnode *from)
{
	return fbdict_merge(into->fbdict, sb->bnode_dict_size,
			    sb->bnode_max_count, from->fbdict);
}

static void adjust_parent_sep(struct cursor *cursor, int level, __be64 newsep)
{
	/* Update separating key until nearest common parent */
	while (level >= 0) {
		struct path_level *parent_at = &cursor->path[level];
		__be64 *this_keyp = level_this_keyp(parent_at);

		assert(0 < be64_to_cpup(this_keyp));
		assert(be64_to_cpup(this_keyp) < be64_to_cpu(newsep));
		log_bnode_adjust(cursor->btree->sb,
				 bufindex(parent_at->buffer),
				 be64_to_cpup(this_keyp),
				 be64_to_cpu(newsep));
		*this_keyp = newsep;
		mark_buffer_unify_non(parent_at->buffer);

		/* The path on parent level is most left? */
		if (parent_at->next - 1 == 0)
			break;

		level--;
	}
}

/* Tracking info for chopped bnode indexes */
struct chopped_index_info {
	tuxkey_t start;
	int count;
};

static void remove_index(struct cursor *cursor, struct chopped_index_info *cii)
{
	struct sb *sb = cursor->btree->sb;
	int level = cursor->level;
	struct path_level *at = &cursor->path[level];
	struct chopped_index_info *ciil = &cii[level];

	/* Collect chopped index in this bnode for logging later */
	if (!ciil->count)
		ciil->start = be64_to_cpup(level_this_keyp(at));
	ciil->count++;

	/* Remove an index */
	bnode_remove_index(sb, level_bnode(at), at->next - 1, 1);
	at->next--;
	mark_buffer_unify_non(at->buffer);

	/*
	 * Climb up to common parent and update separating key.
	 *
	 * What if index is now empty?  (no deleted key)
	 *
	 * Then some key above is going to be deleted and used to set sep
	 * Climb the cursor while at first entry, bail out at root find the
	 * bnode with the old sep, set it to deleted key
	 */

	/* There is no separator for last entry or root bnode */
	if (!level || cursor_level_finished(cursor))
		return;
	/* If removed index was not first entry, no change to separator */
	if (at->next != 0)
		return;

	adjust_parent_sep(cursor, level - 1, *level_next_keyp(at));
}

static int try_leaf_merge(struct btree *btree, struct buffer_head *intobuf,
			  struct buffer_head *frombuf)
{
	struct vleaf *from = bufdata(frombuf);
	struct vleaf *into = bufdata(intobuf);

	/* Try to merge leaves */
	if (btree->ops->leaf_merge(btree, into, from)) {
		struct sb *sb = btree->sb;
		/*
		 * We know frombuf is redirected and dirty. So, in
		 * here, we can just cancel leaf_redirect by bfree(),
		 * instead of defered_bfree()
		 * FIXME: we can optimize freeing leaf without
		 * leaf_redirect, and if we did, this is not true.
		 */
		bfree(sb, bufindex(frombuf), 1);
		log_leaf_free(sb, bufindex(frombuf));
		return 1;
	}
	return 0;
}

static int try_bnode_merge(struct sb *sb, struct buffer_head *intobuf,
			   struct buffer_head *frombuf)
{
	struct bnode *into = bufdata(intobuf);
	struct bnode *from = bufdata(frombuf);

	/* Try to merge bnodes */
	if (bnode_merge_bnodes(sb, into, from)) {
		/*
		 * We know frombuf is redirected and dirty. So, in
		 * here, we can just cancel bnode_redirect by bfree(),
		 * instead of defered_bfree()
		 * FIXME: we can optimize freeing bnode without
		 * bnode_redirect, and if we did, this is not true.
		 */
		bfree(sb, bufindex(frombuf), 1);
		log_bnode_merge(sb, bufindex(frombuf), bufindex(intobuf));
		return 1;
	}
	return 0;
}

/*
 * This is range deletion. So, instead of adjusting balance of the
 * space on sibling bnodes for each change, this just removes the range
 * and merges from right to left even if it is not same parent.
 *
 *              +--------------- (A, B, C)--------------------+
 *              |                    |                        |
 *     +-- (AA, AB, AC) -+       +- (BA, BB, BC) -+      + (CA, CB, CC) +
 *     |        |        |       |        |       |      |       |      |
 * (AAA,AAB)(ABA,ABB)(ACA,ACB) (BAA,BAB)(BBA)(BCA,BCB)  (CAA)(CBA,CBB)(CCA)
 *
 * [less : A, AA, AAA, AAB, AB, ABA, ABB, AC, ACA, ACB, B, BA ... : greater]
 *
 * If we merged from cousin (or re-distributed), we may have to update
 * the index until common parent. (e.g. removed (ACB), then merged
 * from (BAA,BAB) to (ACA), we have to adjust B in root bnode to BB)
 *
 * See, adjust_parent_sep().
 *
 * FIXME: no re-distribute. so, we don't guarantee above than 50%
 * space efficiency. And if range is end of key (truncate() case), we
 * don't need to merge, and adjust_parent_sep().
 *
 * FIXME2: we may want to split chop work for each step. instead of
 * blocking for a long time.
 */
int btree_chop(struct btree *btree, tuxkey_t start, u64 len)
{
	struct sb *sb = btree->sb;
	struct btree_ops *ops = btree->ops;
	struct buffer_head **prev;
	struct chopped_index_info *cii;
	struct cursor *cursor;
	tuxkey_t limit;
	int i, ret, done = 0;

	if (!has_root(btree))
		return 0;

	/* Chop all range if len >= TUXKEY_LIMIT */
	limit = (len >= TUXKEY_LIMIT) ? TUXKEY_LIMIT : start + len;

	prev = kcalloc(btree->root.depth, sizeof(*prev), GFP_NOFS);
	if (prev == NULL)
		return -ENOMEM;

	cii = kcalloc(btree->root.depth, sizeof(*cii), GFP_NOFS);
	if (cii == NULL) {
		ret = -ENOMEM;
		goto error_cii;
	}

	cursor = alloc_cursor(btree, 0);
	if (!cursor) {
		ret = -ENOMEM;
		goto error_alloc_cursor;
	}

	down_write(&btree->lock);
	ret = btree_probe(cursor, start);
	if (ret)
		goto error_btree_probe;

	/* Walk leaves */
	while (1) {
		struct buffer_head *leafbuf;
		tuxkey_t this_key;
		int level = cursor->level;

		/*
		 * FIXME: If leaf was merged and freed later, we don't
		 * need to redirect leaf and leaf_chop()
		 */
		ret = cursor_redirect(cursor);
		if (ret)
			goto out;
		leafbuf = cursor_pop(cursor);

		/* Adjust start and len for this leaf */
		this_key = cursor_level_this_key(cursor);
		if (start < this_key) {
			if (limit < TUXKEY_LIMIT)
				len -= this_key - start;
			start = this_key;
		}

		ret = ops->leaf_chop(btree, start, len, bufdata(leafbuf));
		if (ret) {
			if (ret < 0) {
				blockput(leafbuf);
				goto out;
			}
			mark_buffer_dirty_non(leafbuf);
		}

		/* Try to merge this leaf with prev */
		if (prev[level]) {
			if (try_leaf_merge(btree, prev[level], leafbuf)) {
				trace(">>> can merge leaf %p into leaf %p", leafbuf, prev[level]);
				remove_index(cursor, cii);
				mark_buffer_dirty_non(prev[level]);
				blockput_free(sb, leafbuf);
				goto keep_prev_leaf;
			}
			blockput(prev[level]);
		}
		prev[level] = leafbuf;

keep_prev_leaf:

		if (cursor_level_next_key(cursor) >= limit)
			done = 1;
		/* Pop and try to merge finished bnodes */
		while (done || cursor_level_finished(cursor)) {
			struct buffer_head *buf;
			struct chopped_index_info *ciil;

			level = cursor->level;
			if (level < 0)
				goto chop_root;
			ciil = &cii[level];

			/* Get merge src buffer, and go parent level */
			buf = cursor_pop(cursor);

			/*
			 * Logging chopped indexes
			 * FIXME: If bnode is freed later (e.g. merged),
			 * we dont't need to log this
			 */
			if (ciil->count) {
				log_bnode_del(sb, bufindex(buf), ciil->start,
					      ciil->count);
			}
			memset(ciil, 0, sizeof(*ciil));

			/* Try to merge bnode with prev */
			if (prev[level]) {
				assert(level);
				if (try_bnode_merge(sb, prev[level], buf)) {
					trace(">>> can merge bnode %p into bnode %p", buf, prev[level]);
					remove_index(cursor, cii);
					mark_buffer_unify_non(prev[level]);
					blockput_free_unify(sb, buf);
					goto keep_prev_bnode;
				}
				blockput(prev[level]);
			}
			prev[level] = buf;
keep_prev_bnode:

			if (!level)
				goto chop_root;
		}

		/* Push back down to leaf level */
		do {
			ret = cursor_advance_down(cursor);
			if (ret < 0)
				goto out;
		} while (ret);
	}

chop_root:
	/* Remove depth if possible */
	while (btree->root.depth > 1 && bcount(bufdata(prev[0])) == 1) {
		trace("drop btree level");
		btree->root.block = bufindex(prev[1]);
		btree->root.depth--;
		tux3_mark_btree_dirty(btree);

		/*
		 * We know prev[0] is redirected and dirty. So, in
		 * here, we can just cancel bnode_redirect by bfree(),
		 * instead of defered_bfree()
		 * FIXME: we can optimize freeing bnode without
		 * bnode_redirect, and if we did, this is not true.
		 */
		bfree(sb, bufindex(prev[0]), 1);
		log_bnode_free(sb, bufindex(prev[0]));
		blockput_free_unify(sb, prev[0]);

		vecmove(prev, prev + 1, btree->root.depth);
	}
	ret = 0;

out:
	for (i = 0; i < btree->root.depth; i++) {
		if (prev[i])
			blockput(prev[i]);
	}
	release_cursor(cursor);
error_btree_probe:
	up_write(&btree->lock);

	free_cursor(cursor);
error_alloc_cursor:
	kfree(cii);
error_cii:
	kfree(prev);

	return ret;
}

/* root must be initialized by zero */
static void bnode_init_root(struct sb *sb, struct bnode *root, unsigned count,
			    block_t left, block_t right, tuxkey_t rkey)
{
	fbdict_set_count(root->fbdict, count);
	/* *bnode_keyp(root, 0) = cpu_to_be64(0) */;
	*bnode_blockp(sb, root, 0) = cpu_to_be64(left);
	*bnode_keyp(root, 1) = cpu_to_be64(rkey);
	*bnode_blockp(sb, root, 1) = cpu_to_be64(right);
}

/* Insertion */

static void bnode_add_index(struct sb *sb, struct bnode *bnode, int idx,
			    block_t child, u64 childkey)
{
	__be64 key = cpu_to_be64(childkey);
	__be64 *blockp;

	blockp = fbdict_insert(bnode->fbdict, sb->bnode_dict_size,
			       sb->bnode_max_count, idx, &key);
	BUG_ON(blockp == NULL);
	*blockp = cpu_to_be64(child);
}

static void bnode_split(struct sb *sb, struct bnode *src, int split_idx,
			struct bnode *dst)
{
	fbdict_split(src->fbdict, sb->bnode_dict_size, split_idx, dst->fbdict);
}

/*
 * Insert new leaf to next cursor position.
 * keep == 1: keep current cursor position.
 * keep == 0, set cursor position to new leaf.
 */
static int insert_leaf(struct cursor *cursor, tuxkey_t childkey,
		       struct buffer_head *leafbuf, int keep)
{
	struct btree *btree = cursor->btree;
	struct sb *sb = btree->sb;
	int level = btree->root.depth - 1;
	block_t childblock = bufindex(leafbuf);

	if (keep)
		blockput(leafbuf);
	else {
		cursor_pop_blockput(cursor);
		cursor_push(cursor, leafbuf, CURSOR_LEAF_LEVEL);
	}
	while (level--) {
		struct path_level *at = &cursor->path[level];
		struct bnode *parent = level_bnode(at);
		struct buffer_head *parentbuf = at->buffer;

		/* insert and exit if not full */
		if (bcount(parent) < sb->bnode_max_count) {
			bnode_add_index(sb, parent, at->next, childblock, childkey);
			if (!keep)
				at->next++;
			log_bnode_add(sb, bufindex(parentbuf), childblock, childkey);
			mark_buffer_unify_non(parentbuf);
			cursor_check(cursor);
			return 0;
		}

		/* split a full index bnode */
		struct buffer_head *newbuf = new_bnode(btree);
		if (IS_ERR(newbuf))
			return PTR_ERR(newbuf);

		struct bnode *newnode = bufdata(newbuf);
		unsigned half = bcount(parent) / 2;
		u64 newkey = be64_to_cpup(bnode_keyp(parent, half));

		bnode_split(sb, parent, half, newnode);
		log_bnode_split(sb, bufindex(parentbuf), half, bufindex(newbuf));

		/* if the cursor is in the newnode, use that as the parent */
		int child_is_left = at->next <= half;
		if (!child_is_left) {
			unsigned newnext;
			mark_buffer_unify_non(parentbuf);
			newnext = at->next - half;
			get_bh(newbuf);
			level_replace_blockput(at, newbuf, newnext);
			parentbuf = newbuf;
			parent = newnode;
		} else
			mark_buffer_unify_non(newbuf);

		bnode_add_index(sb, parent, at->next, childblock, childkey);
		if (!keep)
			at->next++;
		log_bnode_add(sb, bufindex(parentbuf), childblock, childkey);
		mark_buffer_unify_non(parentbuf);

		childkey = newkey;
		childblock = bufindex(newbuf);
		blockput(newbuf);

		/*
		 * If child is in left bnode, we should keep the
		 * cursor position to child, otherwise adjust cursor
		 * to new bnode.
		 */
		keep = child_is_left;
	}

	/* Make new root bnode */
	trace("add tree level");
	struct buffer_head *newbuf = new_bnode(btree);
	if (IS_ERR(newbuf))
		return PTR_ERR(newbuf);

	struct bnode *newroot = bufdata(newbuf);
	block_t newrootblock = bufindex(newbuf);
	block_t oldrootblock = btree->root.block;
	int left_bnode = bufindex(cursor->path[0].buffer) != childblock;
	bnode_init_root(sb, newroot, 2, oldrootblock, childblock, childkey);
	cursor_root_add(cursor, newbuf, 1 + !left_bnode);
	log_bnode_root(sb, newrootblock, 2, oldrootblock, childblock, childkey);

	/* Change btree to point the new root */
	btree->root.block = newrootblock;
	btree->root.depth++;

	mark_buffer_unify_non(newbuf);
	tux3_mark_btree_dirty(btree);
	cursor_check(cursor);

	return 0;
}

/* Insert new leaf to next cursor position, then set cursor to new leaf */
int btree_insert_leaf(struct cursor *cursor, tuxkey_t key,
		      struct buffer_head *leafbuf)
{
	return insert_leaf(cursor, key, leafbuf, 0);
}

/*
 * Split leaf, then insert to parent.
 * key:  key to add after split (cursor will point leaf which is including key)
 * hint: hint for split
 *
 * return value:
 *   0 - success
 * < 0 - error
 */
static int btree_leaf_split(struct cursor *cursor, tuxkey_t key, tuxkey_t hint)
{
	trace("split leaf");
	struct btree *btree = cursor->btree;
	struct buffer_head *newbuf;

	newbuf = new_leaf(btree);
	if (IS_ERR(newbuf))
		return PTR_ERR(newbuf);
	log_balloc(btree->sb, bufindex(newbuf), 1);

	struct buffer_head *leafbuf = cursor_leafbuf(cursor);
	tuxkey_t newkey = btree->ops->leaf_split(btree, hint, bufdata(leafbuf),
						 bufdata(newbuf));
	assert(cursor_this_key(cursor) < newkey);
	assert(newkey < cursor_next_key(cursor));
	if (key < newkey)
		mark_buffer_dirty_non(newbuf);
	else
		mark_buffer_dirty_non(leafbuf);
	return insert_leaf(cursor, newkey, newbuf, key < newkey);
}

static int btree_advance(struct cursor *cursor, struct btree_key_range *key)
{
	tuxkey_t limit = cursor_next_key(cursor);
	int skip = 0;

	while (key->start >= limit) {
		int ret = cursor_advance(cursor);
		assert(ret != 0);	/* wrong key range? */
		if (ret < 0)
			return ret;

		limit = cursor_next_key(cursor);
		skip++;
	}
	if (skip > 1) {
		/* key should on next leaf */
		tux3_dbg("skipped more than 1 leaf: why, and probe is better");
		assert(0);
	}

	return 0;
}

int noop_pre_write(struct btree *btree, tuxkey_t key_bottom, tuxkey_t key_limit,
		   void *leaf, struct btree_key_range *key)
{
	return BTREE_DO_DIRTY;
}

int btree_write(struct cursor *cursor, struct btree_key_range *key)
{
	struct btree *btree = cursor->btree;
	struct btree_ops *ops = btree->ops;
	tuxkey_t split_hint;
	int err;

	while (key->len > 0) {
		tuxkey_t bottom, limit;
		void *leaf;
		int ret;

		err = btree_advance(cursor, key);
		if (err)
			return err;	/* FIXME: error handling */

		bottom = cursor_this_key(cursor);
		limit = cursor_next_key(cursor);
		assert(bottom <= key->start && key->start < limit);

		leaf = bufdata(cursor_leafbuf(cursor));
		ret = ops->leaf_pre_write(btree, bottom, limit, leaf, key);
		assert(ret >= 0);
		if (ret == BTREE_DO_RETRY)
			continue;

		if (ret == BTREE_DO_DIRTY) {
			err = cursor_redirect(cursor);
			if (err)
				return err;	/* FIXME: error handling */

			/* Reread leaf after redirect */
			leaf = bufdata(cursor_leafbuf(cursor));
			assert(!ops->leaf_sniff(btree, leaf));

			ret = ops->leaf_write(btree, bottom, limit, leaf, key,
					      &split_hint);
			if (ret < 0)
				return ret;
			if (btree->ops == &dtree_ops)
				remember_dleaf(btree->sb, cursor_leafbuf(cursor));
			if (ret == BTREE_DO_RETRY) {
				mark_buffer_dirty_non(cursor_leafbuf(cursor));
				continue;
			}
		}

		if (ret == BTREE_DO_SPLIT) {
			err = btree_leaf_split(cursor, key->start, split_hint);
			if (err)
				return err;	/* FIXME: error handling */
		}
	}

	return 0;
}

int btree_read(struct cursor *cursor, struct btree_key_range *key)
{
	struct btree *btree = cursor->btree;
	struct btree_ops *ops = btree->ops;
	void *leaf = bufdata(cursor_leafbuf(cursor));
	tuxkey_t bottom = cursor_this_key(cursor);
	tuxkey_t limit = cursor_next_key(cursor);

	/* FIXME: we might be better to support multiple leaves */

	assert(bottom <= key->start && key->start < limit);
	assert(!ops->leaf_sniff(btree, leaf));

	return ops->leaf_read(btree, bottom, limit, leaf, key);
}

void init_btree(struct btree *btree, struct sb *sb, struct root root, struct btree_ops *ops)
{
	btree->sb = sb;
	btree->ops = ops;
	btree->root = root;
	init_rwsem(&btree->lock);
	ops->btree_init(btree);
}

int btree_alloc_empty(struct btree *btree)
{
	struct sb *sb = btree->sb;
	struct buffer_head *leafbuf;
	block_t leafblock;

	assert(!has_root(btree));

	/*
	 * NOTE: If this is dtree and had the direct extent,
	 * new_leaf() will add a extent to leafbuf in ->dleaf_init().
	 * FIXME: better way?
	 */
	leafbuf = new_leaf(btree);
	if (IS_ERR(leafbuf))
		return PTR_ERR(leafbuf);

	leafblock = bufindex(leafbuf);
	trace("leaf at %Lx", leafblock);
	log_balloc(sb, leafblock, 1);

	mark_buffer_dirty_non(leafbuf);
	blockput(leafbuf);

	btree->root = (struct root){ .block = leafblock, .depth = 1 };
	tux3_mark_btree_dirty(btree);

	return 0;
}

/* FIXME: right? and this should be done by btree_chop()? */
int btree_free_empty(struct btree *btree)
{
	struct sb *sb = btree->sb;
	struct btree_ops *ops = btree->ops;
	struct buffer_head *leafbuf;
	block_t leaf;

	if (!has_root(btree))
		return 0;

	assert(btree->root.depth == 1);
	leaf = btree->root.block;
	/* Make btree has no root */
	btree->root = no_root;
	tux3_mark_btree_dirty(btree);

	leafbuf = vol_find_get_block(sb, leaf);
	if (leafbuf && !leaf_need_redirect(sb, leafbuf)) {
		/*
		 * This is redirected leaf. So, in here, we can just
		 * cancel leaf_redirect by bfree(), instead of
		 * defered_bfree().
		 */
		bfree(sb, leaf, 1);
		log_leaf_free(sb, leaf);
		assert(ops->leaf_can_free(btree, bufdata(leafbuf)));
		blockput_free(sb, leafbuf);
	} else {
		defer_bfree(sb, &sb->defree, leaf, 1);
		log_bfree(sb, leaf, 1);
		if (leafbuf) {
			assert(ops->leaf_can_free(btree, bufdata(leafbuf)));
			blockput(leafbuf);
		}
	}

	return 0;
}

int replay_bnode_redirect(struct replay *rp, block_t oldblock, block_t newblock)
{
	struct sb *sb = rp->sb;
	struct buffer_head *newbuf, *oldbuf;
	int err = 0;

	newbuf = vol_getblk(sb, newblock);
	if (!newbuf) {
		err = -ENOMEM;	/* FIXME: error code */
		goto error;
	}
	oldbuf = vol_bread(sb, oldblock);
	if (!oldbuf) {
		err = -EIO;	/* FIXME: error code */
		goto error_put_newbuf;
	}
	assert(!bnode_sniff(bufdata(oldbuf)));

	memcpy(bufdata(newbuf), bufdata(oldbuf), bufsize(newbuf));
	mark_buffer_unify_atomic(newbuf);

	blockput(oldbuf);
error_put_newbuf:
	blockput(newbuf);
error:
	return err;
}

int replay_bnode_root(struct replay *rp, block_t root, unsigned count,
		      block_t left, block_t right, tuxkey_t rkey)
{
	struct sb *sb = rp->sb;
	struct buffer_head *rootbuf;

	rootbuf = vol_getblk(sb, root);
	if (!rootbuf)
		return -ENOMEM;
	bnode_buffer_init(sb, rootbuf);

	bnode_init_root(sb, bufdata(rootbuf), count, left, right, rkey);

	mark_buffer_unify_atomic(rootbuf);
	blockput(rootbuf);

	return 0;
}

/*
 * Before this replay, replay should already dirty the buffer of src.
 * (e.g. by redirect)
 */
int replay_bnode_split(struct replay *rp, block_t src, unsigned pos,
		       block_t dst)
{
	struct sb *sb = rp->sb;
	struct buffer_head *srcbuf, *dstbuf;
	int err = 0;

	srcbuf = vol_getblk(sb, src);
	if (!srcbuf) {
		err = -ENOMEM;	/* FIXME: error code */
		goto error;
	}

	dstbuf = vol_getblk(sb, dst);
	if (!dstbuf) {
		err = -ENOMEM;	/* FIXME: error code */
		goto error_put_srcbuf;
	}
	bnode_buffer_init(sb, dstbuf);

	bnode_split(sb, bufdata(srcbuf), pos, bufdata(dstbuf));

	mark_buffer_unify_non(srcbuf);
	mark_buffer_unify_atomic(dstbuf);

	blockput(dstbuf);
error_put_srcbuf:
	blockput(srcbuf);
error:
	return err;
}

/*
 * Before this replay, replay should already dirty the buffer of bnodeblock.
 * (e.g. by redirect)
 */
static int
replay_bnode_change(struct sb *sb, block_t bnodeblock,
		    u64 val1, u64 val2,
		    void (*change)(struct sb *, struct bnode *, u64, u64))
{
	struct buffer_head *bnodebuf;

	bnodebuf = vol_getblk(sb, bnodeblock);
	if (!bnodebuf)
		return -ENOMEM;	/* FIXME: error code */

	struct bnode *bnode = bufdata(bnodebuf);
	change(sb, bnode, val1, val2);

	mark_buffer_unify_non(bnodebuf);
	blockput(bnodebuf);

	return 0;
}

static void add_func(struct sb *sb, struct bnode *bnode, u64 child, u64 key)
{
	int idx = bnode_lookup(bnode, key) + 1;
	bnode_add_index(sb, bnode, idx, child, key);
}

int replay_bnode_add(struct replay *rp, block_t parent, block_t child,
		     tuxkey_t key)
{
	return replay_bnode_change(rp->sb, parent, child, key, add_func);
}

static void update_func(struct sb *sb, struct bnode *bnode, u64 child, u64 key)
{
	int idx = bnode_lookup(bnode, key);
	assert(be64_to_cpup(bnode_keyp(bnode, idx)) == key);
	*bnode_blockp(sb, bnode, idx) = cpu_to_be64(child);
}

int replay_bnode_update(struct replay *rp, block_t parent, block_t child,
			tuxkey_t key)
{
	return replay_bnode_change(rp->sb, parent, child, key, update_func);
}

int replay_bnode_merge(struct replay *rp, block_t src, block_t dst)
{
	struct sb *sb = rp->sb;
	struct buffer_head *srcbuf, *dstbuf;
	int err = 0, ret;

	srcbuf = vol_getblk(sb, src);
	if (!srcbuf) {
		err = -ENOMEM;	/* FIXME: error code */
		goto error;
	}

	dstbuf = vol_getblk(sb, dst);
	if (!dstbuf) {
		err = -ENOMEM;	/* FIXME: error code */
		goto error_put_srcbuf;
	}

	ret = bnode_merge_bnodes(sb, bufdata(dstbuf), bufdata(srcbuf));
	assert(ret == 1);

	mark_buffer_unify_non(dstbuf);
	mark_buffer_unify_non(srcbuf);

	blockput(dstbuf);
error_put_srcbuf:
	blockput_free_unify(sb, srcbuf);
error:
	return err;
}

static void del_func(struct sb *sb, struct bnode *bnode, u64 key, u64 count)
{
	int idx = bnode_lookup(bnode, key);
	assert(be64_to_cpup(bnode_keyp(bnode, idx)) == key);
	bnode_remove_index(sb, bnode, idx, count);
}

int replay_bnode_del(struct replay *rp, block_t bnode, tuxkey_t key,
		     unsigned count)
{
	return replay_bnode_change(rp->sb, bnode, key, count, del_func);
}

static void adjust_func(struct sb *sb, struct bnode *bnode, u64 from, u64 to)
{
	int idx = bnode_lookup(bnode, from);
	assert(be64_to_cpup(bnode_keyp(bnode, idx)) == from);
	*bnode_keyp(bnode, idx) = cpu_to_be64(to);
}

int replay_bnode_adjust(struct replay *rp, block_t bnode, tuxkey_t from,
			tuxkey_t to)
{
	return replay_bnode_change(rp->sb, bnode, from, to, adjust_func);
}
