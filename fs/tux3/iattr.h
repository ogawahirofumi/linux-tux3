#ifndef TUX3_IATTR_H
#define TUX3_IATTR_H

#define KIND_SIZE		sizeof(__be16)

enum atkind {
	/* Fixed size attrs */
	MODE_OWNER_ATTR	= 0,
	CTIME_SIZE_ATTR	= 1,
	LINK_COUNT_ATTR	= 2,
	MTIME_ATTR	= 3,
	DATA_BTREE_ATTR	= 4,
	/* i_blocks	= 5 */
	/* i_generation	= 6 */
	/* i_version	= 7 */
	/* i_flag	= 8 */
	GENERIC_ATTR	= 9,	/* i_rdev for chr/blk,  parent inum for dir */
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

extern const unsigned atsize[MAX_ATTRS];

#define IATTR_PRESENT_NR	BITS_TO_LONGS(MAX_ATTRS)
struct iattr_req_data {
	struct tux3_iattr_data *idata;		/* inode attributes */
	struct btree *btree;			/* inode dtree */
	struct inode *inode;			/* extended attributes */
	/* attrs info to save */
	unsigned long present[IATTR_PRESENT_NR];
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
