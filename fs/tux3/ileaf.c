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

//#define ILEAF_FORMAT_MULTI_IBASE

#ifndef ILEAF_FORMAT_MULTI_IBASE
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
	/* FIXME: 1<<6 is sane inode size? */
	if (btree->sb->blockbits <= 6)
		btree->entries_per_leaf = 0; /* only for testing */
	else
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

	unsigned i, into_count = from_count - at;
	for (i = 0; i < into_count; i++) {
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
#else /* !ILEAF_FORMAT_MULTI_IBASE */
/*
 * inode leaf format
 *
 *              ileaf
 *     +----------------------+
 *     |       magic          |
 *     |      nr ibase        |
 *     +----------------------+
 *     |  ibase + end of dict |--+ nr inums == end-of-dict - prev end-of-dict
 *     +----------------------+  |
 *     |  ibase + end of dict |-----+
 *     +----------------------+<-+  |
 *     |     offset dict      |-----------+  attrs size = offset - prev offset
 *     +----------------------+<----+     |
 *     |     offset dict      |---------+ |
 *     +----------------------+         | |
 *     |         ...          |         | |
 *     +----------------------+<--------+ |
 *     |                      |           |
 *     |      iattr data      |           |
 *     |                      |           |
 *     +----------------------+<----------+
 *     |                      |
 *     |      iattr data      |
 *     |                      |
 *     +----------------------+
 *
 * A leaf has a small header followed by ibase entries, and dictionary
 * for iattr offsets.  iattr grows down from the top of the leaf top
 * of dictionary.
 *
 * range_of_ibase = ibase_entry[i] - ibase_entry[i - 1]
 * dict at = ibase_entry[i - 1] + (inum - ibase)
 * attr offset = dict[at]
 * attr size = dict[at] - dict[at - 1]
 * attr = end_of_leaf - attr_offset
 */

struct ileaf {
	__be16 magic;		/* Magic number */
	__be16 ibase_count;	/* Number of members in ibase array */
	__be64 head[];
	/* __be64 ibase_and_end_dict[] */
	/* __be16 dict[] */
};

/*
 * If inum is far from ibase more than 6, adding new ibase is smaller
 * than expanding dict[]
 */
#define IBASE_FAR		6

static inline u16 ibase_count(struct ileaf *leaf)
{
	return be16_to_cpu(leaf->ibase_count);
}

static inline u16 dict_read(__be16 *dict)
{
	return be16_to_cpu(*dict);
}

static inline void dict_write(__be16 *dict, u16 attr_offset)
{
	*dict = cpu_to_be16(attr_offset);
}

static inline void dict_add(__be16 *dict, u16 attr_offset_diff)
{
	be16_add_cpu(dict, attr_offset_diff);
}

static inline inum_t ibase_read(void *head)
{
	u64 ibase;
	decode48(head, &ibase);	/* ibase can be unaligned */
	return ibase;
}

static inline void ibase_write(void *head, inum_t ibase)
{
	encode48(head, ibase);
}

static inline int ibase_dictend_read(void *head)
{
	return be16_to_cpu(*(__be16 *)(head + 6));
}

static inline void ibase_dictend_write(void *head, u16 dict_end)
{
	*(__be16 *)(head + 6) = cpu_to_be16(dict_end);
}

static inline void ibase_dictend_add(void *head, u16 diff)
{
	be16_add_cpu((__be16 *)(head + 6), diff);
}

static inline void ibase_ent_write(void *head, inum_t ibase, u16 dict_end)
{
	ibase_write(head, ibase);
	ibase_dictend_write(head, dict_end);
}

/* Calculate number of inums (and dict entries) for this ibase */
static inline u16 ibase_nr(struct ileaf *ileaf, __be64 *head, u16 dict_end)
{
	if (ileaf->head == head)
		return dict_end;
	return dict_end - ibase_dictend_read(head - 1);
}

static inline __be16 *ileaf_dict(struct ileaf *ileaf)
{
	return (__be16 *)(ileaf->head + ibase_count(ileaf));
}

/* Caller must make sure "ibase_count > 0" */
static inline __be16 *ileaf_dict_limit(struct ileaf *ileaf)
{
	u16 dict_end = ibase_dictend_read(ileaf->head + ibase_count(ileaf) - 1);
	return ileaf_dict(ileaf) + dict_end;
}

static inline void *ileaf_attrs_limit(struct btree *btree, struct ileaf *ileaf)
{
	return (void *)ileaf + btree->sb->blocksize;
}

/* Caller must make sure "ibase_count > 0" */
static inline u16 ileaf_attrs_size(struct ileaf *ileaf)
{
	__be16 *dict = ileaf_dict_limit(ileaf) - 1;
	return dict_read(dict);
}

/* Caller must make sure "ibase_count > 0" */
static inline void *ileaf_attrs_start(struct btree *btree, struct ileaf *ileaf)
{
	u16 size = ileaf_attrs_size(ileaf);
	return ileaf_attrs_limit(btree, ileaf) - size;
}

static void ileaf_btree_init(struct btree *btree)
{
	/* FIXME: 1<<6 is sane inode size? */
	if (btree->sb->blockbits <= 6)
		btree->entries_per_leaf = 0; /* only for testing */
	else
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

static int ileaf_sniff(struct btree *btree, void *leaf)
{
	struct ileaf_attr_ops *attr_ops = btree->ops->private_ops;
	if (((struct ileaf *)leaf)->magic != attr_ops->magic)
		return -1;
	return 0;
}

static int ileaf_need(struct ileaf *ileaf)
{
	int count = ibase_count(ileaf);
	if (count > 0) {
		__be16 *dict_limit = ileaf_dict_limit(ileaf);
		size_t head_size = (void *)dict_limit - (void *)ileaf->head;
		return head_size + dict_read(dict_limit - 1);
	}
	return 0;
}

static int ileaf_free(struct btree *btree, struct ileaf *ileaf)
{
	return btree->sb->blocksize - ileaf_need(ileaf) - sizeof(struct ileaf);
}

static int ileaf_can_free(struct btree *btree, void *leaf)
{
	struct ileaf *ileaf = leaf;
	return ibase_count(ileaf) == 0;
}

static inline void ileaf_dump(struct ileaf *ileaf)
{
#if 0
	tux3_dbg("ileaf %p, ibase_count %u", ileaf, ibase_count(ileaf));
	if (ibase_count(ileaf) > 0) {
		__be64 *ibase_head, *ibase_limit = (__be64 *)ileaf_dict(ileaf);
		__be16 *dict;
		u64 ibase;
		u16 i, dict_end, dict_limit, offset;

		dict_end = 0;
		for (i = 0; i < ibase_count(ileaf); i++) {
			u16 nr = ibase_dictend_read(ileaf->head + i) - dict_end;
			dict_end = ibase_dictend_read(ileaf->head + i);
			tux3_dbg("ibase %llu, dict_end %u (nr %u)",
				 ibase_read(ileaf->head + i),
				 dict_end, nr);
		}

		dict = (__be16 *)ibase_limit;
		dict_limit = ibase_dictend_read(ibase_limit - 1);
		ibase_head = ileaf->head;
		ibase = 0;
		offset = dict_end = 0;
		for (i = 0; i < dict_limit; i++) {
			u16 len = dict_read(dict + i) - offset;
			offset = dict_read(dict + i);
			if (i == dict_end) {
				ibase = ibase_read(ibase_head);
				dict_end = ibase_dictend_read(ibase_head);
				ibase_head++;
			}
			tux3_dbg("attrs_offset %u (at %u, inum %llu, len %u)",
				 offset, i, ibase, len);
			ibase++;
		}
	}
#endif
}

struct ileaf_lookup_info {
	inum_t ibase;
	__be64 *ibase_head;
	void *attrs;
	int dict_end, dict_at;
	u16 nr_inums, attrs_offset, attrs_size;
};

/* Find ibase entry >= key. (Caller must make sure "ibase_count > 0") */
static __be64 *ileaf_lookup_ibase(struct ileaf *ileaf, inum_t key)
{
	__be64 *head, *limit;

	/* FIXME: binary search here */
	head = (__be64 *)ileaf->head;
	limit = (__be64 *)ileaf_dict(ileaf);
	while (++head < limit) {
		if (key < ibase_read(head))
			break;
	}
	return head - 1;
}

static void ileaf_lookup_attrs(struct btree *btree, struct ileaf *ileaf,
			       u16 dict_at, struct ileaf_lookup_info *info)
{
	void *attrs_limit = ileaf_attrs_limit(btree, ileaf);
	__be16 *dict = ileaf_dict(ileaf);
	u16 prev_attrs_offset = dict_at ? dict_read(dict + dict_at - 1) : 0;

	info->attrs_offset = dict_read(dict + dict_at);
	info->attrs_size = info->attrs_offset - prev_attrs_offset;
	info->attrs = attrs_limit - info->attrs_offset;
}

/*
 * Find info for inum.
 *
 * return value:
 * 0 - not found (If info->ibase_head != NULL, has ibase info)
 * 1 - found dict entry (If info->attrs_size > 0, info->attrs is available)
 */
static int ileaf_lookup_info(struct btree *btree, inum_t inum,
			     struct ileaf *ileaf,
			     struct ileaf_lookup_info *info)
{
	info->ibase_head = NULL;

	if (ibase_count(ileaf) == 0)
		return 0;

	info->ibase_head = ileaf_lookup_ibase(ileaf, inum);

	/* inum is within ibase range? */
	info->ibase = ibase_read(info->ibase_head);
	info->dict_end = ibase_dictend_read(info->ibase_head);
	info->nr_inums = ibase_nr(ileaf, info->ibase_head, info->dict_end);
	if (inum < info->ibase || info->ibase + info->nr_inums <= inum)
		return 0;

	info->dict_at = info->dict_end - info->nr_inums + (inum - info->ibase);
	ileaf_lookup_attrs(btree, ileaf, info->dict_at, info);
	/* Sanity check: if inum == ibase, should have attrs */
	assert(inum > info->ibase || info->attrs_size > 0);
	/* Sanity check: if inum == end of ibase, should have attrs */
	assert(info->dict_at < info->dict_end - 1 || info->attrs_size > 0);

	return 1;
}

/*
 * Find attrs for inum.
 *
 * return value:
 * NULL - no inum entry (include attrs size == 0)
 * non-NULL - pointer of attrs
 */
static void *ileaf_lookup(struct btree *btree, struct ileaf *ileaf, inum_t inum,
			  u16 *attrs_size)
{
	struct ileaf_lookup_info info;
	if (ileaf_lookup_info(btree, inum, ileaf, &info)) {
		if (info.attrs_size) {
			*attrs_size = info.attrs_size;
			return info.attrs;
		}
	}
	return NULL;
}

int ileaf_inum_exists(struct btree *btree, struct ileaf *ileaf, inum_t inum)
{
	u16 attrs_size;
	return ileaf_lookup(btree, ileaf, inum, &attrs_size) != NULL;
}

static void ileaf_copy(struct btree *btree, struct ileaf *into,
		       struct ileaf *from)
{
	/* Split before first, move all to "into" ileaf */
	void *from_dict_limit = ileaf_dict_limit(from);
	u16 from_attrs_size = ileaf_attrs_size(from);
	void *from_attrs_limit = ileaf_attrs_limit(btree, from);
	void *into_attrs_limit = ileaf_attrs_limit(btree, into);
	int size;

	/* Copy ibase entries + dict */
	size = (void *)from_dict_limit - (void *)from;
	memcpy(into, from, size);

	/* Copy attrs */
	memcpy(into_attrs_limit - from_attrs_size,
	       from_attrs_limit - from_attrs_size,
	       from_attrs_size);
}

/* Strip 0-size dict entries at end of ibase entry. */
static int dict_strip_tail(__be16 *start, __be16 *limit)
{
	int strip = 0;

	if (limit > start) {
		u16 prev_offset, offset;

		limit--;
		offset = dict_read(limit);
		while (--limit >= start) {
			prev_offset = dict_read(limit);
			if (prev_offset != offset)
				break;
			strip++;
		}
	}

	return strip;
}

/* Strip 0-size dict entries at head of ibase entry. */
static int dict_strip_head(__be16 *start, __be16 *limit, __be16 *head)
{
	u16 prev_offset = head > start ? dict_read(head - 1) : 0;
	__be16 *orig = head;

	while (head < limit) {
		u16 offset = dict_read(head);
		if (prev_offset != offset)
			break;
		prev_offset = offset;
		head++;
	}

	return head - orig;
}

static tuxkey_t ileaf_split(struct btree *btree, tuxkey_t hint,
			    void *vfrom, void *vinto)
{
	struct ileaf *from = vfrom, *into = vinto;
	inum_t split_inum = hint;
	struct ileaf_lookup_info info;
	__be16 *dict, *from_dict, *dict_limit;
	void *from_attrs_start;
	int has_dict, split_at_middle, from_count, into_count, size, diff;
	u16 prev_offset;

	assert(!ileaf_sniff(btree, from));

	/* If from doesn't have anything, nothing to do */
	from_count = ibase_count(from);
	if (from_count == 0)
		goto out;

	from_dict = ileaf_dict(from);
	dict_limit = ileaf_dict_limit(from);

	has_dict = ileaf_lookup_info(btree, split_inum, from, &info);
	if (!has_dict) {
		if (split_inum < info.ibase) {
			/* Split before first, move all to "into" ileaf */
			ileaf_copy(btree, into, from);
			from->ibase_count = 0;
			goto out;
		}
		if ((__be64 *)from_dict == info.ibase_head + 1 &&
		    split_inum >= info.ibase + info.nr_inums) {
			/* Split after last, do nothing */
			goto out;
		}

		/*
		 * Split at middle of ibase entries, but doesn't have
		 * dict entry. Copy from next ibase entry.
		 */
		info.dict_at = info.dict_end;
		info.ibase_head++;
		info.ibase = split_inum;
		ileaf_lookup_attrs(btree, from, info.dict_at, &info);
	}

	split_at_middle = info.ibase != split_inum;
	/* Remember start of attrs in "from" */
	from_attrs_start = ileaf_attrs_start(btree, from);

	dict = from_dict + info.dict_at;
	prev_offset = info.dict_at ? dict_read(dict - 1) : 0;
	if (has_dict && split_at_middle) {
		int strip;
		/* Strip 0-size dict entries from copy for "into" */
		strip = dict_strip_head(from_dict, dict_limit, dict);

		split_inum += strip;
		dict += strip;
	}
	/*
	 * Calculate the number of ibase entries. If split at the
	 * middle of an ibase entry, need this ibase for "from" ileaf too.
	 */
	into_count = (__be64 *)from_dict - info.ibase_head;
	BUG_ON(!into_count);	/* Should have 1 entry, at least */
	into->ibase_count = cpu_to_be16(into_count);
	/* Copy ibase entries into "into" with adjust */
	{
		int i;
		diff = from_dict - dict;
		for (i = 0; i < into_count; i++) {
			inum_t ibase = ibase_read(info.ibase_head + i);
			u16 dict_end = ibase_dictend_read(info.ibase_head + i);
			ibase_ent_write(into->head + i, ibase, dict_end + diff);
		}
		/* Adjust ibase of first */
		if (split_at_middle)
			ibase_write(into->head, split_inum);
	}
	/* Copy dict to "into" with adjust */
	{
		__be16 *into_dict = (__be16 *)(into->head + into_count);
		__be16 *p = dict;
		while (p < dict_limit) {
			u16 offset = dict_read(p);
			dict_write(into_dict, offset - prev_offset);
			p++;
			into_dict++;
		}
	}

	/* Copy attrs */
	size = (info.attrs + info.attrs_size) - from_attrs_start;
	if (size) {
		void *into_attrs_start = ileaf_attrs_limit(btree, into) - size;
		memcpy(into_attrs_start, from_attrs_start, size);
	}

	if (has_dict && split_at_middle) {
		int strip;
		/* Strip 0-size dict entries from "from" ileaf */
		strip = dict_strip_tail(from_dict, from_dict + info.dict_at);

		info.dict_at -= strip;
		ibase_dictend_write(info.ibase_head, info.dict_at);
	}
	/* Remove copied ibase entries from "from" */
	if (into_count - split_at_middle > 0) {
		from_count -= into_count - split_at_middle;
		from->ibase_count = cpu_to_be16(from_count);

		size = info.dict_at * sizeof(__be16);
		memmove(from->head + from_count, from_dict, size);
	}
out:
	ileaf_dump(from);
	ileaf_dump(into);
	return hint;
}

/* Merge from right leaf (vfrom) into left leaf (vinto) */
static int ileaf_merge(struct btree *btree, void *vinto, void *vfrom)
{
	struct ileaf *into = vinto, *from = vfrom;
	__be64 *ibase_head, *ibase_limit, *from_ibase_head;
	__be16 *dict_start, *dict_limit;
	inum_t ibase, from_ibase;
	u16 into_count, from_count, nr_inums, dict_end, into_attrs_size;
	int i, size, ibase_diff, dict_end_diff;

	/* No entry in "into" */
	into_count = ibase_count(into);
	if (into_count == 0) {
		/* Move all from "into" to "from" */
		ileaf_copy(btree, into, from);
		from->ibase_count = 0;
		return 1;
	}
	/* No entry in "from" */
	from_count = ibase_count(from);
	if (from_count == 0) {
		/* Nothing to do */
		return 1;
	}

	ileaf_dump(from);
	ileaf_dump(into);

	/* Read last entry of ibase in "into" */
	dict_start = ileaf_dict(into);
	ibase_head = (__be64 *)dict_start - 1;
	ibase_limit = (__be64 *)dict_start;
	ibase = ibase_read(ibase_head);
	dict_end = ibase_dictend_read(ibase_head);
	nr_inums = ibase_nr(into, ibase_head, dict_end);
	dict_limit = dict_start + dict_end;

	/* Read first entry of ibase  in "from" */
	from_ibase_head = from->head;
	from_ibase = ibase_read(from_ibase_head);

	/* Must be into < from */
	BUG_ON(ibase >= from_ibase);

	/* Check whether how merge */
	if (ibase + nr_inums + IBASE_FAR <= from_ibase) {
		/* ibase is far, merge as is */
		ibase_diff = 0;
		dict_end_diff = 0;
	} else {
		/* ibase is close, merge last and first ibase entries */
		ibase_diff = from_ibase - (ibase + nr_inums);
		dict_end_diff = ibase_dictend_read(from_ibase_head);
		from_count--;
		from_ibase_head++;
	}

	{
		/* Check free space */
		int need_size = ileaf_need(from);
		need_size += ibase_diff * sizeof(__be16);
		need_size -= dict_end_diff ? sizeof(__be64) : 0;
		if (ileaf_free(btree, from) < need_size)
			return 0;	/* No space */
	}

	{
		/* Copy attrs before modify leaf */
		u16 from_attrs_size = ileaf_attrs_size(from);
		void *attrs_limit = ileaf_attrs_limit(btree, into);

		into_attrs_size = ileaf_attrs_size(into);

		memcpy(attrs_limit - into_attrs_size - from_attrs_size,
		       ileaf_attrs_limit(btree, from) - from_attrs_size,
		       from_attrs_size);
	}

	/* Set ibase_count */
	into->ibase_count = cpu_to_be16(into_count + from_count);

	/* Make space to copy ibase entries */
	{
		void *orig = dict_start;
		size = (void *)dict_limit - (void *)dict_start;
		dict_start = (__be16 *)((__be64 *)dict_start + from_count);
		dict_limit = (void *)dict_start + size;
		memmove(dict_start, orig, size);
	}
	/* Copy ibase entries with adjust */
	for (i = 0; i < from_count; i++) {
		u16 from_dict_end = ibase_dictend_read(from_ibase_head + i);
		from_ibase = ibase_read(from_ibase_head + i);

		/* Append "from" ibase entries to "into" */
		ibase_ent_write(ibase_limit + i,
				from_ibase,
				dict_end + ibase_diff + from_dict_end);
	}
	/* Expand dict, and merge ibase of last on "into" and first on "from" */
	if (ibase_diff) {
		ibase_dictend_add(ibase_limit - 1, ibase_diff);
		while (ibase_diff--) {
			dict_write(dict_limit, into_attrs_size);
			dict_limit++;
		}
	}
	if (dict_end_diff)
		ibase_dictend_add(ibase_limit - 1, dict_end_diff);

	{
		/* Copy dict */
		__be16 *from_dict = ileaf_dict(from);
		__be16 *from_dict_limit = ileaf_dict_limit(from);
		while (from_dict < from_dict_limit) {
			u16 from_offset = dict_read(from_dict);
			dict_write(dict_limit, into_attrs_size + from_offset);

			dict_limit++;
			from_dict++;
		}
	}

	from->ibase_count = 0;

	ileaf_dump(from);
	ileaf_dump(into);

	return 1;
}

static u16 dict_read_prev(__be16 *start, __be16 *dict)
{
	if (start == dict)
		return 0;
	return dict_read(dict - 1);
}

/*
 * Chop specified range of dict entries and attrs.
 *
 * If zero==1, chop dict entries by setting 0-size.
 * If zero==0, chop dict dict entries by memmove.
 */
static __be16 *chop_dict_and_attrs(struct btree *btree, struct ileaf *ileaf,
				   __be16 *dict_start, __be16 *dict_limit,
				   u16 start_at, u16 limit_at, int zero)
{
	__be16 *dict = dict_start + start_at;
	__be16 *limit = dict_start + limit_at;
	int offset_start = dict_read_prev(dict_start, dict);
	int offset_end = dict_read_prev(dict_start, limit);
	int offset_diff = offset_end - offset_start;
	int attrs_diff, attrs_size;
	void *attrs_start;

	/* Chop attrs before changing dict */
	attrs_size = ileaf_attrs_size(ileaf);
	attrs_start = ileaf_attrs_limit(btree, ileaf) - attrs_size;
	attrs_diff = offset_end - offset_start;
	memmove(attrs_start + attrs_diff, attrs_start, attrs_size - offset_end);

	if (zero) {
		/* Chop dict entries by setting 0-size offset */
		while (dict < limit) {
			dict_write(dict, offset_start);
			dict++;
		}
	} else {
		/* Chop dict entries by memmove */
		int size = (void *)dict_limit - (void *)limit;
		memmove(dict, limit, size);
		/* Set new dict_limit */
		dict_limit = (void *)dict + size;
	}

	/* Adjust offset for rest entries */
	while (dict < dict_limit) {
		dict_add(dict, -offset_diff);
		dict++;
	}

	return dict_limit;	/* New dict_limit */
}

/*
 * Chop specified range of ibase entries.
 * nr: number of chopping ibase entries
 * nr_partial: partial dict chop on ibase at end of chop
 */
static void chop_ibase_entries(struct ileaf *ileaf, __be16 *dict_limit,
			       __be64 *head, int nr, int dictend_diff)
{
	__be64 *head2, *limit;

	if (!dictend_diff)
		return;

	/* Remove ibase entries */
	head2 = head + nr;
	memmove(head, head2, (void *)dict_limit - (void *)head2);

	/* Adjust ibase count */
	be16_add_cpu(&ileaf->ibase_count, -nr);

	/* Adjust dict_end of rest ibase entries */
	limit = ileaf->head + ibase_count(ileaf);
	while (head < limit) {
		ibase_dictend_add(head, -dictend_diff);
		head++;
	}
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
	struct ileaf_lookup_info s_info, e_info;
	__be16 *dict_start, *dict_limit;
	int s_partial, e_partial, chop_by_zero, split_ibase;
	int nr_chop_head, dictend_diff;
	inum_t end;

	/* No data, does nothing */
	if (ibase_count(ileaf) == 0)
		return 0;

	end = start + len;
	dict_start = ileaf_dict(ileaf);
	dict_limit = ileaf_dict_limit(ileaf);

	/* Find start of chop */
	if (!ileaf_lookup_info(btree, start, ileaf, &s_info)) {
		/* If didn't have dict, fill info for "start" */
		u64 diff;

		if (start < s_info.ibase) {
			/* "start" is before this ibase, use this ibase entry */
			s_info.dict_at = s_info.dict_end - s_info.nr_inums;
		} else {
			/* "start" is after this ibase, use next ibase entry */
			if (s_info.ibase_head + 1 >= (__be64 *)dict_start)
				return 0;

			s_info.dict_at = s_info.dict_end;
			s_info.ibase_head++;
			s_info.ibase = ibase_read(s_info.ibase_head);
			s_info.dict_end = ibase_dictend_read(s_info.ibase_head);
			s_info.nr_inums = s_info.dict_end - s_info.dict_at;
		}

		/* Adjust start and len at ibase entry */
		diff = s_info.ibase - start;
		if (diff >= len)
			return 0; /* end is before first ibase entry */
		start += diff;
		len -= diff;

		ileaf_lookup_attrs(btree, ileaf, s_info.dict_at, &s_info);
	}

	/* Find end of chop */
	if (!ileaf_lookup_info(btree, end, ileaf, &e_info)) {
		/* If didn't have dict, fill info for "end" */
		BUG_ON(end < e_info.ibase); /* This case was checked already */

		/* "end" is after this ibase, stop at dict_end */
		e_info.dict_at = e_info.dict_end;
		ileaf_lookup_attrs(btree, ileaf, e_info.dict_at, &e_info);
	}

	if (s_info.ibase == start) {
		/* Chop from start of this ibase entry. */
		s_partial = 0;
	} else {
		/* s_info.ibase < start. Partial chop at first ibase entry. */
		__be16 *chop_start = dict_start + s_info.dict_at;
		int strip;
		/* If 0-size dict entries before this, includes those
		 * to chop range */
		strip = dict_strip_tail(dict_start, chop_start);

		/* Include 0-size dict entries */
		s_info.dict_at -= strip;
		start -= strip;
		len += strip;

		/* Left this ibase */
		s_partial = 1;
	}
	if (end == e_info.ibase + e_info.nr_inums) {
		/* Chop until end of this ibase entry */
		e_partial = 0;
	} else {
		/* end < e_info.ibase + e_info.nr_inums.
		 * Partial chop at last ibase entry. */
		__be16 *chop_limit = dict_start + e_info.dict_at;
		int strip;
		/* If 0-size dict entries after this, includes those
		 * to chop range */
		strip = dict_strip_head(dict_start, dict_limit, chop_limit);

		/* Include 0-size dict entries */
		e_info.dict_at += strip;
		len += strip;
		end += strip;

		/* Left this ibase */
		e_partial = 1;
	}
#if 0
	if (!s_partial && !e_partial) {
		/* Remove ibase entries */
		tux3_dbg("1");
	} else if (s_partial && !e_partial) {
		/* Adjust dict_end in this ibase entry */
		tux3_dbg("2");
	} else if (!s_partial && e_partial) {
		/* Adjust ibase in this ibase entry */
		tux3_dbg("3");
	} else /*if (s_partial && e_partial)*/ {
		if (len < IBASE_FAR) {
			if (s_info.ibase_head == e_info.ibase_head) {
				/* Use contiguous dict */
				tux3_dbg("4");
			} else {
				/* Even if ibase entries are close, we
				 * don't merge ibase entries in chop path. */
				tux3_dbg("5");
			}
		} else {
			if (s_info.ibase_head == e_info.ibase_head) {
				/* Need new ibase entry to separate dict */
				tux3_dbg("6");
			} else {
				/* Reuse s_info's and e_info's ibase entry */
				tux3_dbg("7");
			}
		}
	}
#endif
	chop_by_zero = 0;
	split_ibase = 0;
	if (s_partial && e_partial && s_info.ibase_head == e_info.ibase_head) {
		if (len < IBASE_FAR) {
			/* Using contiguous dict is smaller than
			 * adding/adjusting ibase entries */
			chop_by_zero = 1;
		} else {
			/* Add new ibase entry to separate dict */
			split_ibase = 1;
		}
	}
	/*
	 * Chop dict/attrs first, to avoid run out of space by new
	 * ibase.  FIXME: better to merge with chop of ibase entries
	 * to reduce total memmove() size.
	 */
	dict_limit = chop_dict_and_attrs(btree, ileaf, dict_start, dict_limit,
					 s_info.dict_at, e_info.dict_at,
					 chop_by_zero);

	if (chop_by_zero) {
		/* No need to change to ibase entries */
		goto out;
	}

	if (split_ibase) {
		int size;

		/* Make space to add new ibase entry after this */
		be16_add_cpu(&ileaf->ibase_count, 1);
		e_info.ibase_head++;
		size = (void *)dict_limit - (void *)e_info.ibase_head;
		memmove(e_info.ibase_head + 1, e_info.ibase_head, size);

		/* Set fake ibase dict_end, adjusted by chop ibase entries */
		ibase_dictend_write(e_info.ibase_head, e_info.dict_end);
		/* For dictend_diff (Adjust dict_end from e_info) */
		s_info.dict_end = s_info.dict_at;
		e_info.ibase = start;
		BUG_ON(!e_partial); /* ibase is written later */
	}

	/* Chop ibase entries */
	if (s_partial) {
		/* Adjust dict_end in this ibase entry */
		ibase_dictend_write(s_info.ibase_head, s_info.dict_at);
		s_info.ibase_head++;
		dictend_diff = s_info.dict_end - s_info.dict_at;
	} else {
		/* Set prev dict_end to s_info.dict_end for dictend_diff */
		s_info.dict_end = (ileaf->head == s_info.ibase_head ?
				   0 : ibase_dictend_read(s_info.ibase_head-1));
		dictend_diff = 0;
	}
	if (e_partial) {
		/* Adjust ibase in this ibase entry */
		ibase_write(e_info.ibase_head, end);
		dictend_diff += end - e_info.ibase;
	} else {
		/* Remove this ibase entry too */
		e_info.ibase_head++;
	}
	if (e_info.ibase_head > s_info.ibase_head) {
		/* Calculate number of dict entries by chopping ibase entries */
		u16 dict_end2 = ibase_dictend_read(e_info.ibase_head - 1);
		dictend_diff += dict_end2 - s_info.dict_end;
		nr_chop_head = e_info.ibase_head - s_info.ibase_head;
	} else
		nr_chop_head = 0;

	chop_ibase_entries(ileaf, dict_limit, s_info.ibase_head, nr_chop_head,
			   dictend_diff);

out:
	ileaf_dump(ileaf);

	return 1;
}

static tuxkey_t ileaf_split_hint(struct btree *btree,
				 tuxkey_t key_bottom, tuxkey_t key_limit,
				 struct ileaf *ileaf, tuxkey_t key, int size)
{
	/*
	 * FIXME: make sure there is space for size.
	 * FIXME: better split position?
	 */

	__be64 *p, *limit = (__be64 *)ileaf_dict(ileaf);
	int dict_end, prev_dict_end;

	p = limit - 1;
	dict_end = ibase_dictend_read(p);
	if (key >= ibase_read(p) + ibase_nr(ileaf, p, dict_end)) {
		/* FIXME: optimization for linear allocation, better way? */
		return key;
	}

	/* Split at center of dict. FIXME: better to use center of attrs? */
	dict_end /= 2;
	prev_dict_end = 0;
	for (p = ileaf->head; p < limit; p++) {
		if (dict_end - ibase_dictend_read(p) <= 0)
			break;
		prev_dict_end = ibase_dictend_read(p);
	}
	return ibase_read(p) + (dict_end - prev_dict_end);
}

/* Try merge specified ibase and next ibase entry */
static void resize_try_merge_ibase(struct btree *btree, struct ileaf *ileaf,
				   __be64 *head)
{
	__be64 *ibase_limit = (__be64 *)ileaf_dict(ileaf);
	u64 ibase;
	u16 nr_inums;

	if (head + 1 >= ibase_limit)
		return;

	ibase = ibase_read(head);
	nr_inums = ibase_nr(ileaf, head, ibase_dictend_read(head));
	if (ibase + nr_inums == ibase_read(head + 1)) {
		/* If 2 ibase entries are contiguous, merge */
		void *dict_limit = ileaf_dict_limit(ileaf);
		ibase_dictend_write(head, ibase_dictend_read(head + 1));
		memmove(head + 1, head + 2, dict_limit - (void *)(head + 2));
		be16_add_cpu(&ileaf->ibase_count, -1);
	}
}

/* Make space to write new attrs */
static void *ileaf_resize(struct btree *btree, struct ileaf *ileaf,
			  tuxkey_t inum, int newsize)
{
	int count = ibase_count(ileaf);
	struct ileaf_lookup_info info;
	int has_dict, attrs_size_diff, dict_cnt, expand_direction;
	__be64 *ibase_head, *try_merge = NULL;
	__be16 *dict, *dict_limit;

	/*
	 * Resize is done by 3 steps.
	 *
	 * 1) Collect positions and size to adjust ibase, dict, and attrs
	 * 2) Check available free space
	 * 3) Adjust/Modify ibase, dict, and attrs
	 */

	ileaf_dump(ileaf);

	/* 1) Collect Positions and size */
	has_dict = ileaf_lookup_info(btree, inum, ileaf, &info);
	if (has_dict) {
		if (info.attrs_size) {
			/* Attrs exists */
			attrs_size_diff = newsize - (int)info.attrs_size;
		} else {
			/* Dict entry exists , but attrs size == 0 */
			attrs_size_diff = newsize;
		}
		ibase_head = NULL;	/* Don't add new ibase */
		dict_cnt = 0;		/* Don't add new dict */
		expand_direction = 0;
	} else {
		attrs_size_diff = newsize;

		/*
		 * Doesn't have dict[], how to expand ibase and dict?
		 */

		/* ibase entry exists */
		if (count) {
			if (inum < info.ibase) {
				int diff = info.ibase - inum;
				if (diff < IBASE_FAR) {
					/* Prepend dict to this exists ibase */
					ibase_head = NULL;
					dict_cnt = diff;
				} else {
					/* Add new ibase before this ibase */
					ibase_head = info.ibase_head;
					dict_cnt = 1;
				}
				expand_direction = -1;
			} else {
				int diff = inum - (info.ibase+info.nr_inums-1);
				if (diff < IBASE_FAR) {
					/* Append dict to this exists ibase */
					ibase_head = NULL;
					try_merge = info.ibase_head;
					dict_cnt = diff;
				} else {
					/* Add new ibase after this ibase */
					ibase_head = info.ibase_head + 1;
					try_merge = info.ibase_head + 1;
					dict_cnt = 1;
				}
				expand_direction = 1;
			}
		} else {
			/* No ibase entry exists */
			ibase_head = (__be64 *)ileaf->head;
			dict_cnt = 1;
			expand_direction = -1;
		}

		/* Set start position and prev dict_end to expand dict */
		if (expand_direction < 0) {
			if (info.ibase_head > ileaf->head) {
				__be64 *prev = info.ibase_head - 1;
				info.dict_at = ibase_dictend_read(prev);
				info.dict_end = info.dict_at;
			} else {
				info.dict_at = 0;
				info.dict_end = 0;
			}
		} else {
			info.dict_at = ibase_dictend_read(info.ibase_head);
			info.dict_end = info.dict_at;
		}
		/* Set position to add attrs */
		if (info.dict_at - 1 >= 0) {
			ileaf_lookup_attrs(btree, ileaf, info.dict_at-1, &info);
		} else {
			info.attrs = ileaf_attrs_limit(btree, ileaf);
			info.attrs_offset = 0;
			info.attrs_size = 0;
		}
	}

	{
		/* 2) Check free space */
		int need_size;
		need_size = ibase_head ? sizeof(*ibase_head) : 0;
		need_size += sizeof(*dict) * dict_cnt;
		need_size += attrs_size_diff;
		if (ileaf_free(btree, ileaf) < need_size)
			return NULL;	/* No space */
	}

	/* 3) Adjust/Modify */

	/* Expand/Shrink attrs space */
	if (count && attrs_size_diff) {
		void *attrs_start = ileaf_attrs_start(btree, ileaf);
		int size = info.attrs - attrs_start;
		memmove(attrs_start - attrs_size_diff, attrs_start, size);
	}

	dict = ileaf_dict(ileaf) + info.dict_at;
	dict_limit = count ? ileaf_dict_limit(ileaf) : dict;

	/* Make space for new ibase entry */
	if (ibase_head) {
		int size = (void *)dict_limit - (void *)ibase_head;

		be16_add_cpu(&ileaf->ibase_count, 1);
		/* Make space for new ibase entry, then write */
		memmove(ibase_head + 1, ibase_head, size);

		/* Start ibase position to adjust dict_end */
		info.ibase_head = ibase_head + 1;

		/* Adjust position for expanding dict */
		dict = (void *)dict + sizeof(*ibase_head);
		dict_limit = (void *)dict_limit + sizeof(*ibase_head);
	}
	if (ibase_head) {
		/* Adjust ibase entry for new ibase */
		ibase_ent_write(ibase_head, inum, info.dict_end + dict_cnt);
	} else if (expand_direction < 0) {
		/* Adjust ibase for prepend dict */
		ibase_write(info.ibase_head, inum);
	}

	/* Add new dict entries */
	if (dict_cnt) {
		__be64 *ibase_limit = (__be64 *)ileaf_dict(ileaf);
		int size = (void *)dict_limit - (void *)dict;
		u16 attrs_offset = info.attrs_offset;
		__be16 *limit;

		/* Adjust dict_end in ibase entries */
		while (info.ibase_head < ibase_limit) {
			ibase_dictend_add(info.ibase_head, dict_cnt);
			info.ibase_head++;
		}

		/* Make space for new dict entry, then write */
		memmove(dict + dict_cnt, dict, size);
		dict_limit += dict_cnt;

		/* Fill value on expanded entries of dict */
		if (expand_direction < 0) {
			/* Prepend dict case, write new attrs offset at first */
			limit = dict + dict_cnt;
			attrs_offset += newsize;
			dict_write(dict, attrs_offset);
			dict++;
		} else
			limit = dict + dict_cnt - 1;
		/* Fill attrs offset */
		while (dict < limit) {
			dict_write(dict, attrs_offset);
			dict++;
		}
		if (expand_direction > 0) {
			/* Prepend dict case, write new attrs offset at last */
			attrs_offset += newsize;
			dict_write(dict, attrs_offset);
			dict++;
		}
	}
	/* Adjust dict entries to new offset */
	if (attrs_size_diff) {
		while (dict < dict_limit) {
			dict_add(dict, attrs_size_diff);
			dict++;
		}
	}

	/*
	 * FIXME: Better to merge the merge ibase entries to above, to
	 * reduce total memmove size.
	 */
	if (try_merge)
		resize_try_merge_ibase(btree, ileaf, try_merge);

	ileaf_dump(ileaf);
	return info.attrs - attrs_size_diff;
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

	ileaf_dump(ileaf);
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
	struct ileaf_lookup_info info;
	__be16 *dict_start, *dict, *dict_limit;

	ileaf_dump(ileaf);

	key_limit = min(key + len, key_limit);

	/* If there is no attrs for this key, it is free */
	if (!ileaf_lookup_info(btree, key, ileaf, &info))
		goto out;
	if (info.attrs_size == 0)
		goto out;

	/* This key has attrs, find free inum from next. */
	key++;
	if (key >= key_limit)
		goto out;

	dict_start = ileaf_dict(ileaf);
	dict = dict_start + info.dict_at + 1;
	dict_limit = dict_start + info.dict_end;
	while (1) {
		/* Find 0 size attrs */
		while (dict < dict_limit) {
			u16 attrs_offset = dict_read(dict);
			if (attrs_offset - info.attrs_offset == 0)
				goto out;
			info.attrs_offset = attrs_offset;

			key++;
			if (key >= key_limit)
				goto out;
			dict++;
		}

		info.ibase_head++;
		if (info.ibase_head >= (__be64 *)dict_start)
			break;

		/* Find next ibase */
		info.ibase = ibase_read(info.ibase_head);
		info.dict_end = ibase_dictend_read(info.ibase_head);
		/* key is between 2 ibase entries */
		if (key < info.ibase)
			break;
		BUG_ON(key != info.ibase);

		dict_limit = dict_start + info.dict_end;
	}
out:
	if (key < key_limit) {
		*(inum_t *)data = key;
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
	struct ileaf_enumrate_cb *cb = data;
	struct ileaf_lookup_info info;
	__be16 *dict_start, *dict, *dict_limit;
	void *attrs_limit;

	ileaf_dump(ileaf);

	key_limit = min(key + len, key_limit);
	dict_start = ileaf_dict(ileaf);

	if (!ileaf_lookup_info(btree, key, ileaf, &info)) {
		/* No ibase entries */
		if (!info.ibase_head)
			return 0;

		if (key < info.ibase) {
			/* key < ibase, find inum from this ibase entry */
			key = info.ibase;
			info.dict_at = info.dict_end - info.nr_inums;
			/* Set prev attrs_offset */
			info.attrs_offset = 0;
		} else {
			/* key >= ibase + nr, find inum from next ibase entry */

			/* No next ibase entry */
			if (info.ibase_head + 1 >= (__be64 *)ileaf_dict(ileaf))
				return 0;

			info.dict_at = info.dict_end;
			info.ibase_head++;
			key = ibase_read(info.ibase_head);
			info.dict_end = ibase_dictend_read(info.ibase_head);
			/* Set prev attrs_offset */
			info.attrs_offset =
				dict_read(dict_start + info.dict_at - 1);
		}
	} else {
		/* Has dict entry, set prev attrs_offset */
		info.attrs_offset -= info.attrs_size;
	}

	dict = dict_start + info.dict_at;
	dict_limit = dict_start + info.dict_end;
	attrs_limit = ileaf_attrs_limit(btree, ileaf);

	while (key < key_limit) {
		while (dict < dict_limit) {
			u16 prev_offset = info.attrs_offset;
			unsigned size;

			info.attrs_offset = dict_read(dict);
			size = info.attrs_offset - prev_offset;
			if (size > 0) {
				void *attrs = attrs_limit - info.attrs_offset;
				int err = cb->callback(btree, key, attrs, size,
						       cb->data);
				if (err)
					return err;
			}

			key++;
			if (key >= key_limit)
				goto out;
			dict++;
		}

		info.ibase_head++;
		if (info.ibase_head >= (__be64 *)dict_start)
			break;

		/* Enumerate next ibase */
		key = ibase_read(info.ibase_head);
		dict_limit = dict_start + ibase_dictend_read(info.ibase_head);
	}

out:
	return 0;
}
#endif /* !ILEAF_FORMAT_MULTI_IBASE */
