#ifndef TUX3_IATTR_H
#define TUX3_IATTR_H

enum atkind {
	/* Fixed size attrs */
	GENERIC_ATTR	= 0,	/* i_rdev for chr/blk,  parent inum for dir */
	MODE_OWNER_ATTR	= 1,
	DATA_BTREE_ATTR	= 2,
	CTIME_SIZE_ATTR	= 3,
	LINK_COUNT_ATTR	= 4,
	MTIME_ATTR	= 5,
	/* i_blocks	= 6 */
	/* i_generation	= 7 */
	/* i_version	= 8 */
	/* i_flag	= 9 */
	RESERVED1_ATTR	= 10,
	VAR_ATTRS,
	/* Variable size (extended) attrs */
	IDATA_ATTR	= 11,
	XATTR_ATTR	= 12,
	/* acl		= 13 */
	/* allocation hint = 14 */
	RESERVED2_ATTR	= 15,
	MAX_ATTRS,
};

enum atbit {
	/* Fixed size attrs */
	GENERIC_BIT	= 1 << GENERIC_ATTR,
	MODE_OWNER_BIT	= 1 << MODE_OWNER_ATTR,
	CTIME_SIZE_BIT	= 1 << CTIME_SIZE_ATTR,
	DATA_BTREE_BIT	= 1 << DATA_BTREE_ATTR,
	LINK_COUNT_BIT	= 1 << LINK_COUNT_ATTR,
	MTIME_BIT	= 1 << MTIME_ATTR,
	/* Variable size (extended) attrs */
	IDATA_BIT	= 1 << IDATA_ATTR,
	XATTR_BIT	= 1 << XATTR_ATTR,
};

extern unsigned atsize[MAX_ATTRS];

struct iattr_req_data {
	struct tux3_iattr_data *idata;		/* inode attributes */
	struct btree *btree;			/* inode dtree */
	struct inode *inode;			/* extended attributes */
};

/* Decode from on-disk to inode field */
static inline bool iattr_decode_generic(struct inode *inode, u64 generic)
{
	switch (inode->i_mode & S_IFMT) {
	case S_IFBLK:
	case S_IFCHR:
		inode->i_rdev = huge_decode_dev(generic);
		return true;
	case S_IFDIR:
		tux_inode(inode)->parent_inum = generic;
		return true;
	}
	return false;
}

/* Encode from inode field to on-disk */
static inline u64 iattr_encode_generic(struct inode *inode)
{
	switch (inode->i_mode & S_IFMT) {
	case S_IFBLK:
	case S_IFCHR:
		return huge_encode_dev(inode->i_rdev);
	case S_IFDIR:
		return tux_inode(inode)->parent_inum;
	}
	return 0;
}
#endif /* !TUX3_IATTR_H */
