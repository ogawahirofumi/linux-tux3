/*
 * Inode attributes.
 *
 * Copyright (c) 2008-2014 Daniel Phillips
 * Copyright (c) 2008-2014 OGAWA Hirofumi
 */

#include "tux3.h"
#include "ileaf.h"
#include "iattr.h"

/*
 * Variable size attribute format:
 *
 *    immediate data: kind+version:16, bytes:16, data[bytes]
 *    immediate xattr: kind+version:16, bytes:16, atom:16, data[bytes - 2]
 */

const unsigned atsize[MAX_ATTRS] = {
	/* Fixed size attrs */
	[MODE_OWNER_ATTR]	= 10,
	[CTIME_SIZE_ATTR]	= 16,
	/* Conditional Fixed size attrs */
	[LINK_COUNT_ATTR]	= 4,
	[MTIME_ATTR]		= 8,
	[DATA_BTREE_ATTR]	= 8,
	[GENERIC_ATTR]		= 8,

	/* Variable size (extended) attrs */
	[IDATA_ATTR]		= 2,
	[XATTR_ATTR]		= 4,
};

/*
 * Tux3 times are nanosecond from base FS time (for now, same with unix epoch).
 * It is not clear whether the saved space is worth the lower precision.
 */

static inline struct timespec64 spectime(const s64 time)
{
	return ns_to_timespec64(time);
}

static inline s64 tuxtime(const struct timespec64 ts)
{
	return timespec64_to_ns(&ts);
}

/* unused */
int attr_check(void *attrs, unsigned size)
{
	void *limit = attrs + size;
	unsigned head;

	while (attrs < limit - 1) {
		attrs = decode16(attrs, &head);
		unsigned kind = head >> 12;
		if (kind >= MAX_ATTRS)
			return 0;
		if (attrs + atsize[kind] > limit)
			return 0;
		attrs += atsize[kind];
	}
	return 1;
}

/*
 * If attr is default value, we don't need to save those attrs.
 * This should match with cond_decode()
 */
static int cond_encode_asize(struct iattr_req_data *iattr_data)
{
	struct tux3_iattr_data *idata = iattr_data->idata;
	int size = 0;

	/* generic == 0 by default */
	if (idata->generic) {
		__set_bit(GENERIC_ATTR, iattr_data->present);
		size += KIND_SIZE + atsize[GENERIC_ATTR];
	}

	/* no_root by default */
	if (!has_no_root(iattr_data->btree)) {
		__set_bit(DATA_BTREE_ATTR, iattr_data->present);
		size += KIND_SIZE + atsize[DATA_BTREE_ATTR];
	}

	/* dir->nlink == 2 or other->nlink == 1 by default */
	if (!(S_ISDIR(idata->i_mode) && idata->i_nlink == 2) &&
	    !(!S_ISDIR(idata->i_mode) && idata->i_nlink == 1)) {
		__set_bit(LINK_COUNT_ATTR, iattr_data->present);
		size += KIND_SIZE + atsize[LINK_COUNT_ATTR];
	}

	/* same with ctime by default */
	if (!timespec64_equal(&idata->i_ctime, &idata->i_mtime)) {
		__set_bit(MTIME_ATTR, iattr_data->present);
		size += KIND_SIZE + atsize[MTIME_ATTR];
	}

	return size;
}

/*
 * Set default value if attr is not presented.
 * This should match with cond_encode_asize()
 */
static int cond_decode(struct inode *inode, unsigned long *present, u64 generic)
{
	struct sb *sb = tux_sb(inode->i_sb);
	int err = 0;

	if (!test_bit(LINK_COUNT_ATTR, present)) {
		if (S_ISDIR(inode->i_mode))
			set_nlink(inode, 2);
		else
			set_nlink(inode, 1);
	}

	if (!test_bit(MTIME_ATTR, present))
		inode->i_mtime = inode->i_ctime;

	if (!test_bit(DATA_BTREE_ATTR, present))
		init_btree(&tux_inode(inode)->btree, sb, no_root, &dtree_ops);

	if (test_bit(GENERIC_ATTR, present)) {
		if (!iattr_decode_generic(inode, generic)) {
			/* FIXME: report corruption? */
			return -1;
		}
	} else {
		inode->i_rdev = 0;
		tux_inode(inode)->parent_inum = 0;
	}

	return err;
}

/* Calculate size and present. */
static int encode_asize(struct iattr_req_data *iattr_data)
{
	int size = 0;

	/* Attributes which is always presented */
	__set_bit(MODE_OWNER_ATTR, iattr_data->present);
	size += KIND_SIZE + atsize[MODE_OWNER_ATTR];
	__set_bit(CTIME_SIZE_ATTR, iattr_data->present);
	size += KIND_SIZE + atsize[CTIME_SIZE_ATTR];
	/* i_version; */

	size += cond_encode_asize(iattr_data);

	return size;
}

void *encode_kind(void *attrs, unsigned kind, unsigned version)
{
	return encode16(attrs, (kind << 12) | version);
}

static void *encode_attrs(struct btree *btree, void *data, void *attrs,
			  unsigned size)
{
	struct sb *sb = btree->sb;
	struct iattr_req_data *iattr_data = data;
	struct tux3_iattr_data *idata = iattr_data->idata;
	struct btree *attr_btree = iattr_data->btree;
	void *limit = attrs + size;
	int kind;

	for_each_set_bit(kind, iattr_data->present, VAR_ATTRS) {
		BUG_ON(attrs + KIND_SIZE + atsize[kind] > limit);

		attrs = encode_kind(attrs, kind, sb->version);
		switch (kind) {
		case MODE_OWNER_ATTR:
			attrs = encode16(attrs, idata->i_mode);
			attrs = encode32(attrs, idata->i_uid);
			attrs = encode32(attrs, idata->i_gid);
			break;
		case CTIME_SIZE_ATTR:
			attrs = encode64(attrs, tuxtime(idata->i_ctime));
			attrs = encode64(attrs, idata->i_size);
			break;
		case LINK_COUNT_ATTR:
			attrs = encode32(attrs, idata->i_nlink);
			break;
		case MTIME_ATTR:
			attrs = encode64(attrs, tuxtime(idata->i_mtime));
			break;
		case DATA_BTREE_ATTR:
			attrs = encode64(attrs, pack_root(&attr_btree->root));
			break;
		case GENERIC_ATTR:
			attrs = encode64(attrs, idata->generic);
			break;
		}
	}
	return attrs;
}

void *decode_kind(void *attrs, unsigned *kind, unsigned *version)
{
	unsigned head;
	attrs = decode16(attrs, &head);
	*version = head & 0xfff;
	*kind = head >> 12;
	return attrs;
}

static void *decode_attrs(struct inode *inode, void *attrs, unsigned size)
{
	struct sb *sb = tux_sb(inode->i_sb);
	struct tux3_inode *tuxnode = tux_inode(inode);
	unsigned long present[IATTR_PRESENT_NR] = {};
	void *limit = attrs + size;
	u64 v64, generic = 0;
	u32 v32;

	while (attrs + KIND_SIZE < limit) {
		unsigned version, kind;

		attrs = decode_kind(attrs, &kind, &version);
		if (attrs + atsize[kind] > limit)
			break;
		if (version != sb->version) {
			attrs += atsize[kind];
			continue;
		}

		switch (kind) {
		case MODE_OWNER_ATTR:
			attrs = decode16(attrs, &v32);
			inode->i_mode = v32;
			attrs = decode32(attrs, &v32);
			i_uid_write(inode, v32);
			attrs = decode32(attrs, &v32);
			i_gid_write(inode, v32);
			break;
		case CTIME_SIZE_ATTR:
			attrs = decode64(attrs, &v64);
			inode->i_ctime = spectime(v64);
			attrs = decode64(attrs, &v64);
			inode->i_size = v64;
			break;
		case LINK_COUNT_ATTR:
			attrs = decode32(attrs, &v32);
			set_nlink(inode, v32);
			break;
		case MTIME_ATTR:
			attrs = decode64(attrs, &v64);
			inode->i_mtime = spectime(v64);
			break;
		case DATA_BTREE_ATTR:
			attrs = decode64(attrs, &v64);
			init_btree(&tuxnode->btree, sb, unpack_root(v64),
				   &dtree_ops);
			break;
		case GENERIC_ATTR:
			attrs = decode64(attrs, &generic);
			break;
		case XATTR_ATTR:
			attrs = decode_xattr(inode, attrs);
			break;
		default:
			/* FIXME: report corruption? */
			goto error;
		}

		__set_bit(kind, present);
	}
	if (attrs != limit) {
		/* FIXME: attribute size is strange. report corruption? */
		goto error;
	}

	if (cond_decode(inode, present, generic) < 0)
		goto error;

	return attrs;

error:
	return NULL;
}

/* Calculate size and present to save */
static int iattr_encoded_size(struct btree *btree, void *data)
{
	struct iattr_req_data *iattr_data = data;
	return encode_asize(iattr_data) + encode_xsize(iattr_data->inode);
}

static void iattr_encode(struct btree *btree, void *data, void *attrs, int size)
{
	struct iattr_req_data *iattr_data = data;
	struct inode *inode = iattr_data->inode;
	void *attr;

	attr = encode_attrs(btree, data, attrs, size);
	attr = encode_xattrs(inode, attr, attrs + size - attr);
	assert(attr == attrs + size);
}

static int iattr_decode(struct btree *btree, void *data, void *attrs, int size)
{
	struct inode *inode = data;
	unsigned xsize;

	xsize = decode_xsize(inode, attrs, size);
	if (xsize) {
		int err = new_xcache(inode, xsize);
		if (err)
			return err;
	}

	if (decode_attrs(inode, attrs, size) == NULL)
		return -EIO;

	if (tux_inode(inode)->xcache)
		xcache_dump(inode);

	return 0;
}

struct ileaf_attr_ops iattr_ops = {
	.magic		= cpu_to_be16(TUX3_MAGIC_ILEAF),
	.encoded_size	= iattr_encoded_size,
	.encode		= iattr_encode,
	.decode		= iattr_decode,
};
