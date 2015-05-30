/*
 * Inode btree leaf operations.
 *
 * Copyright (c) 2008-2015 Daniel Phillips
 * Copyright (c) 2008-2015 OGAWA Hirofumi
 */

#include "tux3.h"
#include "ileaf.h"

#ifndef trace
#define trace trace_on
#endif

/*
 * inode leaf format
 *
 * A leaf has a small header followed by a vector of offset.  A table of
 * attributes within the block grows down from the top of the leaf towards the
 * top of the offsets vector, indexed by the difference between inum and
 * leaf->ibase, the base inum of the table block.
 */

struct ileaf {
	__be16 magic;		/* Magic number */
	__be16 count;		/* Counts of used offset info entries */
	u32 pad;
	__be64 ibase;		/* Base inode number */
	char table[];		/* ileaf data: offset info ... inode attrs  */
};

static inline __be16 *ileaf_dict(struct ileaf *ileaf)
{
	return (__be16 *)ileaf->table;
}

static inline void *ileaf_attrs_limit(struct btree *btree, struct ileaf *ileaf)
{
	return (void *)ileaf + btree->sb->blocksize;
}

static inline __be16 dict_read_be(__be16 *dict)
{
	return *dict;
}

static inline unsigned dict_read(__be16 *dict)
{
	return be16_to_cpu(*dict);
}

static inline __be16 dict_read_prev_be(__be16 *dict, unsigned at)
{
	return at ? *(dict + at - 1) : 0;
}

static inline unsigned dict_read_prev(__be16 *dict, unsigned at)
{
	return at ? dict_read(dict + at - 1) : 0;
}

static inline void dict_write(__be16 *dict, int n)
{
	*dict = cpu_to_be16(n);
}

static inline void dict_multi_write(__be16 *dict, int n, int count)
{
	__be16 n_be = cpu_to_be16(n);
	int i;
	for (i = 0; i < count; i++)
		*(dict + i) = n_be;
}

static inline void dict_add(__be16 *dict, int n)
{
	be16_add_cpu(dict, n);
}

static inline unsigned ileaf_count(struct ileaf *ileaf)
{
	return be16_to_cpu(ileaf->count);
}

static inline inum_t ileaf_ibase(struct ileaf *ileaf)
{
	return be64_to_cpu(ileaf->ibase);
}

static void ileaf_btree_init(struct btree *btree)
{
	btree->entries_per_leaf = 1 << (btree->sb->blockbits - 6);
}

static int ileaf_init(struct btree *btree, void *leaf)
{
	struct ileaf_attr_ops *attr_ops = btree->ops->private_ops;
	*(struct ileaf *)leaf = (struct ileaf){
		.magic = attr_ops->magic,
	};
	return 0;
}

static int ileaf_need(struct ileaf *ileaf)
{
	unsigned count = ileaf_count(ileaf);
	if (count) {
		__be16 *dict = ileaf_dict(ileaf);
		return dict_read(dict + count - 1) + count * sizeof(*dict);
	}
	return 0;
}

static int ileaf_free(struct btree *btree, struct ileaf *ileaf)
{
	return btree->sb->blocksize
		- ileaf_need(ileaf) - sizeof(struct ileaf);
}

static int ileaf_sniff(struct btree *btree, void *leaf)
{
	struct ileaf_attr_ops *attr_ops = btree->ops->private_ops;
	if (((struct ileaf *)leaf)->magic != attr_ops->magic)
		return -1;
	return 0;
}

static int ileaf_can_free(struct btree *btree, void *leaf)
{
	struct ileaf *ileaf = leaf;
	return ileaf_count(ileaf) == 0;
}

static inline void ileaf_dump(struct ileaf *ileaf)
{
#if 0
	tux3_dbg("ileaf %p, ibase %llu, count %u",
		 ileaf, ileaf_ibase(ileaf), ileaf_count(ileaf));
	if (ileaf_count(ileaf) > 0) {
		__be16 *dict = ileaf_dict(ileaf);
		inum_t ibase = ileaf_ibase(ileaf);
		unsigned at;
		u16 prev;

		prev = 0;
		for (at = 0; at < ileaf_count(ileaf); at++) {
			u16 offset = dict_read(dict + at);
			tux3_dbg("offset %u (at %u, inum %llu, len %u)",
				 offset, at, ibase + at, offset - prev);
			prev = offset;
		}
	}
#endif
}

static void *ileaf_lookup(struct btree *btree, struct ileaf *ileaf, inum_t inum,
			  u16 *attrs_size)
{
	int count = ileaf_count(ileaf);
	void *attrs = NULL;

	if (count) {
		inum_t ibase = ileaf_ibase(ileaf);
		if (ibase <= inum && inum < ibase + count) {
			unsigned size, at = inum - ibase;
			__be16 *dict = ileaf_dict(ileaf);
			unsigned offset = dict_read(dict + at);

			size = offset - dict_read_prev(dict, at);
			if (size) {
				void *limit = ileaf_attrs_limit(btree, ileaf);
				attrs = limit - offset;
			}

			*attrs_size = size;
		}
	}
	return attrs;
}

int ileaf_inum_exists(struct btree *btree, struct ileaf *ileaf, inum_t inum)
{
	u16 attrs_size;
	return ileaf_lookup(btree, ileaf, inum, &attrs_size) != NULL;
}

/* Trim trailing empty dict entries */
static unsigned ileaf_trim_tail(struct ileaf *ileaf, unsigned count)
{
	__be16 *dict = ileaf_dict(ileaf);
	__be16 offset_be;

	if (count == 0)
		return 0;

	offset_be = dict_read_be(dict + count - 1);
	while (count) {
		if (offset_be != dict_read_prev_be(dict, count - 1))
			break;
		count--;
	}

	return count;
}

/* Skip empty dict entries */
static unsigned ileaf_skip_empty(struct ileaf *ileaf, unsigned at)
{
	unsigned count = ileaf_count(ileaf);
	if (at < count) {
		__be16 *dict = ileaf_dict(ileaf);
		__be16 prev_be = dict_read_prev_be(dict, at);
		while (at < count) {
			if (prev_be != dict_read_be(dict + at))
				break;
			at++;
		}
	}
	return at;
}

static tuxkey_t ileaf_split(struct btree *btree, tuxkey_t hint,
			    void *vfrom, void *vinto)
{
#define SPLIT_AT_INUM
	struct ileaf *from = vfrom, *into = vinto;
	u64 from_ibase = ileaf_ibase(from);
	unsigned from_count = ileaf_count(from);
	__be16 *from_dict = ileaf_dict(from);
	__be16 *into_dict = ileaf_dict(into);

	assert(!ileaf_sniff(btree, from));
#ifdef SPLIT_AT_INUM
	if (from_count == 0 || hint >= from_ibase + from_count) {
		/* Nothing to do */
		into->count = 0;
		goto out;
	}
	unsigned split_at = max(hint, from_ibase) - from_ibase;
#else
	if (from_count == 0) {
		/* Nothing to do */
		into->count = 0;
		goto out;
	}
	/* binsearch inum starting nearest middle of block */
	unsigned at = 1, hi = from_count;
	while (at < hi) {
		int mid = (at + hi) / 2;
		if (dict_read(dict, mid + 1) < (btree->sb->blocksize / 2))
			at = mid + 1;
		else
			hi = mid;
	}
#endif
	/* Trim leading empty inodes */
	unsigned at = ileaf_skip_empty(from, split_at);
	unsigned split = dict_read_prev(from_dict, at);
	unsigned attrs_size = dict_read(from_dict + from_count - 1);
	unsigned size = attrs_size - split;
	void *from_limit = ileaf_attrs_limit(btree, from);
	void *from_attrs = from_limit - attrs_size;
	void *into_attrs = ileaf_attrs_limit(btree, into) - size;
	assert(attrs_size >= split);
	memcpy(into_attrs, from_attrs, size);

	unsigned into_count = from_count - at;
	for (int i = 0; i < into_count; i++) {
		u16 offset = dict_read(from_dict + at + i);
		dict_write(into_dict + i, offset - split);
	}
	into->ibase = cpu_to_be64(from_ibase + at);
	into->count = cpu_to_be16(into_count);

	/* Trim trailing empty inodes */
	at = ileaf_trim_tail(from, split_at);
	from->count = cpu_to_be16(at);
#if 0
	{
		void *p = from_dict + at;
		memset(p, 0, from_limit - dict_read_prev(from_dict, at) - p);
	}
#endif

out:
	ileaf_dump(from);
	ileaf_dump(into);

	return hint;
}

static int ileaf_merge(struct btree *btree, void *vinto, void *vfrom)
{
	struct ileaf *into = vinto, *from = vfrom;
	inum_t into_ibase = ileaf_ibase(into);
	inum_t from_ibase = ileaf_ibase(from);
	unsigned into_count = ileaf_count(into);
	unsigned from_count = ileaf_count(from);

	/* If "from" is empty, does nothing */
	if (!from_count)
		return 1;

	BUG_ON(into_count > 0 && from_ibase < into_ibase + into_count);
	ileaf_dump(into);
	ileaf_dump(from);

	__be16 *into_dict = ileaf_dict(into);
	__be16 *from_dict = ileaf_dict(from);
	unsigned into_size = 0;
	if (into_count) {
		int hole = from_ibase - (into_ibase + into_count);
		int need_size = hole * sizeof(*into_dict) + ileaf_need(from);

		if (ileaf_free(btree, into) < need_size)
			return 0;

		/* Fill hole of dict until from_ibase */
		into_size = dict_read(into_dict + into_count - 1);
		dict_multi_write(into_dict + into_count, into_size, hole);

		into_count += hole;
	} else {
		/* Copy "from" as is */
		into->ibase = from->ibase;
	}

	/* Merge attrs data from "from" */
	unsigned from_size = dict_read(from_dict + from_count - 1);
	void *into_attrs = ileaf_attrs_limit(btree, into) - into_size;
	void *from_attrs = ileaf_attrs_limit(btree, from) - from_size;
	memcpy(into_attrs - from_size, from_attrs, from_size);

	/* Merge dict from "from" */
	memcpy(into_dict+into_count, from_dict, from_count*sizeof(*from_dict));

	/* Adjust merged dict from "from" */
	if (into_size) {
		int i;
		for (i = into_count; i < into_count + from_count; i++)
			dict_add(into_dict + i, into_size);
	}

	into->count = cpu_to_be16(into_count + from_count);
//	from->count = 0;

	ileaf_dump(into);

	return 1;
}

/*
 * Chop inums
 * return value:
 * < 0 - error
 *   1 - modified
 *   0 - not modified
 */
static int ileaf_chop(struct btree *btree, tuxkey_t start, u64 len, void *leaf)
{
	struct ileaf *ileaf = leaf;
	__be16 *dict = ileaf_dict(ileaf);
	inum_t ibase = ileaf_ibase(ileaf);
	unsigned count = ileaf_count(ileaf);
	u16 start_off, end_off, attrs_size;
	void *attrs;
	unsigned at, size;

	ileaf_dump(ileaf);

	if (count == 0 || start + len <= ibase || ibase + count <= start)
		return 0;

	if (start < ibase) {
		len -= ibase - start;
		start = ibase;
	}
	at = start - ibase;
	len = min_t(u64, len, count - at);

	start_off = dict_read_prev(dict, at);
	end_off = dict_read_prev(dict, at + len);
	if (start_off == end_off)
		return 0;

	/* Remove data */
	size = end_off - start_off;
	attrs_size = dict_read(dict + count - 1);
	attrs = ileaf_attrs_limit(btree, ileaf) - attrs_size;
	memmove(attrs + size, attrs, attrs_size - end_off);

	if (at == 0) {
		/* Chop from ibase */
		at = ileaf_skip_empty(ileaf, at + len);

		/* Remove dict entries */
		count -= at;
		memmove(dict, dict + at, count * sizeof(*dict));
		ileaf->ibase = cpu_to_be64(ibase + at);
		ileaf->count = cpu_to_be16(count);

		at = 0;
		len = 0;
	} else if (at + len >= count) {
		/* Chop until end of dict */
		at = ileaf_trim_tail(ileaf, at);
		count = at;
		ileaf->count = cpu_to_be16(count);

		len = 0;
	}

	/* Fill empty dict */
	if (len) {
		dict_multi_write(dict + at, start_off, len);
		at += len;
	}
	/* Adjust dict */
	while (at < count) {
		dict_add(dict + at, -size);
		at++;
	}

	ileaf_dump(ileaf);

	return 1;
}

static void *ileaf_resize(struct btree *btree, struct ileaf *ileaf,
			  tuxkey_t inum, int newsize)
{
	inum_t ibase = ileaf_ibase(ileaf);
	unsigned count = ileaf_count(ileaf);
	__be16 *dict = ileaf_dict(ileaf);
	void *attrs, *attrs_limit;
	inum_t at;
	int extend_dict, offset, size, attrs_size;

	ileaf_dump(ileaf);

	/* Set new ibase if have no ibase */
	if (count == 0) {
		ileaf->ibase = cpu_to_be64(inum);
		ileaf->count = cpu_to_be16(1);
		dict_write(dict, newsize);
		attrs = ileaf_attrs_limit(btree, ileaf) - newsize;
		goto out;
	}

	/* Get existent attributes, and calculate expand/shrink size */
	if (inum < ibase) {
		at = ibase - inum;

		/* Check size roughly to avoid overflow int */
		if (at * sizeof(*dict) >= btree->sb->blocksize)
			goto overflow;

		/* Need to prepend dict */
		extend_dict = at * sizeof(*dict);
		offset = 0;
		size = 0;
	} else {
		at = inum - ibase;

		if (at + 1 > count) {
			/* Check size roughly to avoid overflow int */
			if ((at + 1) * sizeof(*dict) >= btree->sb->blocksize)
				goto overflow;

			/* Need to extend dict */
			extend_dict = (at + 1 - count) * sizeof(*dict);
			offset = dict_read(dict + count - 1);
			size = 0;
		} else {
			/* "at" is in dict, so get attr size */
			extend_dict = 0;
			offset = dict_read(dict + at);
			size = offset - dict_read_prev(dict, at);
		}
	}

	if (ileaf_free(btree, ileaf) < newsize - size + extend_dict) {
overflow:
		return NULL;
	}

	attrs_size = dict_read_prev(dict, count);

	if (inum < ibase) {
		/* Prepend dict */
		memmove(dict + at, dict, count * sizeof(*dict));
		dict_multi_write(dict, newsize, at);
		count += at;
		ileaf->ibase = cpu_to_be64(inum);
		ileaf->count = cpu_to_be16(count);
	} else if (extend_dict) {
		/* Extend dict */
		dict_multi_write(dict + count, offset, at + 1 - count);
		count = at + 1;
		ileaf->count = cpu_to_be16(count);
	}

	attrs_limit = ileaf_attrs_limit(btree, ileaf);
	attrs = attrs_limit - offset;
	if (newsize != size) {
		/* Expand/Shrink attr space */
		void *attrs_start = attrs_limit - attrs_size;
		int diff = newsize - size;

		memmove(attrs_start - diff, attrs_start, attrs_size - offset);

		/* Adjust dict */
		while (at < count) {
			dict_add(dict + at, diff);
			at++;
		}

		attrs -= diff;
	}

out:
	ileaf_dump(ileaf);

	return attrs;
}

/* Decide split position. */
static tuxkey_t ileaf_split_hint(struct btree *btree,
				 tuxkey_t key_bottom, tuxkey_t key_limit,
				 struct ileaf *ileaf, tuxkey_t key, int size)
{
	/*
	 * FIXME: make sure there is space for size.
	 * FIXME: better split position?
	 */
	inum_t ibase = ileaf_ibase(ileaf);
	unsigned count = ileaf_count(ileaf);
	inum_t hint = ibase + count;

	if (key >= hint && key_limit >= hint + btree->entries_per_leaf) {
		/* FIXME: optimization for linear allocation, better way? */
		hint = max(hint, ibase + 1);
	} else {
		if (key < ibase) {
			if ((ibase - key) >= 2)
				hint = (ibase + key) / 2;
			else
				hint = ibase + (count / 2);
		} else {
			if (key >= hint && (key - hint) >= 2)
				hint = (hint + key) / 2;
			else
				hint = ibase + max(count / 2, 1U);
		}
	}

	if (hint <= key_bottom || hint >= key_limit) {
		/* hint have to split range of key */
		tux3_dbg("btree %p, bottom %llu, limit %llu, ileaf %p, "
			 "ibase %llu, count %u, key %llu, size %u, hint %llu",
			 btree, key_bottom, key_limit, ileaf, ibase, count,
			 key, size, hint);
		BUG_ON(1);
	}

	return hint;
}

/*
 * Write inode attributes.
 */
static int ileaf_write(struct btree *btree, tuxkey_t key_bottom,
		       tuxkey_t key_limit,
		       void *leaf, struct btree_key_range *key,
		       tuxkey_t *split_hint)
{
	struct ileaf_req *rq = container_of(key, struct ileaf_req, key);
	struct ileaf_attr_ops *attr_ops = btree->ops->private_ops;
	struct ileaf *ileaf = leaf;
	void *attrs;
	int size;

	assert(key->len == 1);

	size = attr_ops->encoded_size(btree, rq->data);
	assert(size);

	attrs = ileaf_resize(btree, ileaf, key->start, size);
	if (attrs == NULL) {
		/* There is no space to store */
		*split_hint = ileaf_split_hint(btree, key_bottom, key_limit,
					       ileaf, key->start, size);
		return BTREE_DO_SPLIT;
	}

	attr_ops->encode(btree, rq->data, attrs, size);

	key->start++;
	key->len--;

	return BTREE_DO_RETRY;
}

static int ileaf_read(struct btree *btree, tuxkey_t key_bottom,
		      tuxkey_t key_limit,
		      void *leaf, struct btree_key_range *key)
{
	struct ileaf_req *rq = container_of(key, struct ileaf_req, key);
	struct ileaf_attr_ops *attr_ops = btree->ops->private_ops;
	struct ileaf *ileaf = leaf;
	void *attrs;
	u16 size;

	attrs = ileaf_lookup(btree, ileaf, key->start, &size);
	if (attrs == NULL)
		return -ENOENT;

	return attr_ops->decode(btree, rq->data, attrs, size);
}

struct btree_ops itree_ops = {
	.btree_init	= ileaf_btree_init,
	.leaf_init	= ileaf_init,
	.leaf_split	= ileaf_split,
	.leaf_merge	= ileaf_merge,
	.leaf_chop	= ileaf_chop,
	.leaf_pre_write	= noop_pre_write,
	.leaf_write	= ileaf_write,
	.leaf_read	= ileaf_read,
	.private_ops	= &iattr_ops,

	.leaf_sniff	= ileaf_sniff,
	.leaf_can_free	= ileaf_can_free,
};

struct btree_ops otree_ops = {
	.btree_init	= ileaf_btree_init,
	.leaf_init	= ileaf_init,
	.leaf_split	= ileaf_split,
	.leaf_merge	= ileaf_merge,
	.leaf_chop	= ileaf_chop,
	.leaf_pre_write	= noop_pre_write,
	.leaf_write	= ileaf_write,
	.leaf_read	= ileaf_read,
	.private_ops	= &oattr_ops,

	.leaf_sniff	= ileaf_sniff,
	.leaf_can_free	= ileaf_can_free,
};

/*
 * Find free inum
 * (callback for btree_traverse())
 *
 * return value:
 * 1 - found
 * 0 - not found
 */
int ileaf_find_free(struct btree *btree, tuxkey_t key_bottom,
		    tuxkey_t key_limit, void *leaf,
		    tuxkey_t key, u64 len, void *data)
{
	struct ileaf *ileaf = leaf;
	inum_t at, ibase = ileaf_ibase(ileaf);
	unsigned count = ileaf_count(ileaf);

	ileaf_dump(ileaf);

	if (count == 0 || key < ibase) {
		*(inum_t *)data = key;
		return 1;
	}

	at = key - ibase;
	key_limit = min(key_limit, key + len);
	if (at < count) {
		__be16 *dict = ileaf_dict(leaf);
		__be16 prev_be = dict_read_prev_be(dict, at);

		while (at < count) {
			__be16 offset_be = dict_read_be(dict + at);
			if (offset_be == prev_be)
				break;
			at++;
			prev_be = offset_be;
		}
	}

	if (ibase + at < key_limit) {
		*(inum_t *)data = ibase + at;
		return 1;
	}

	return 0;
}

/*
 * Enumerate inum
 * (callback for btree_traverse())
 */
int ileaf_enumerate(struct btree *btree, tuxkey_t key_bottom,
		    tuxkey_t key_limit, void *leaf,
		    tuxkey_t key, u64 len, void *data)
{
	struct ileaf *ileaf = leaf;
	__be16 *dict = ileaf_dict(ileaf);
	struct ileaf_enumrate_cb *cb = data;
	inum_t at, ibase = ileaf_ibase(ileaf);
	unsigned count = ileaf_count(ileaf);

	ileaf_dump(ileaf);

	if (count == 0)
		goto out;

	if (key < ibase) {
		len -= min(ibase - key, len);
		key = ibase;
	}
	at = key - ibase;
	count = min_t(u64, key + len - ibase, count);

	if (at < count) {
		void *attrs_limit = ileaf_attrs_limit(btree, ileaf);
		unsigned prev = dict_read_prev(dict, at);
		for (; at < count; at++) {
			unsigned size, offset;
			inum_t inum;
			void *attrs;
			int err;

			offset = dict_read(dict + at);
			if (offset == prev)
				continue;
			attrs = attrs_limit - offset;
			size = offset - prev;

			inum = ibase + at;
			err = cb->callback(btree, inum, attrs, size, cb->data);
			if (err)
				return err;

			prev = offset;
		}
	}

out:
	return 0;
}
