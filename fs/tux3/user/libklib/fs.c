#include <tux3user.h>

/* depending on tux3 */

/*
 * Attribute stuff
 */

int setattr_prepare(struct user_namespace *mnt_userns, struct dentry *dentry,
		    struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	unsigned int ia_valid = attr->ia_valid;

	/*
	 * First check size constraints.  These can't be overriden using
	 * ATTR_FORCE.
	 */
	if (ia_valid & ATTR_SIZE) {
		int error = inode_newsize_ok(inode, attr->ia_size);
		if (error)
			return error;
	}

	/* If force is set do it anyway. */
	if (ia_valid & ATTR_FORCE)
		goto kill_priv;

#ifdef __KERNEL__
	/* Make sure a caller can chown. */
	if ((ia_valid & ATTR_UID) && !chown_ok(mnt_userns, inode, attr->ia_uid))
		return -EPERM;

	/* Make sure caller can chgrp. */
	if ((ia_valid & ATTR_GID) && !chgrp_ok(mnt_userns, inode, attr->ia_gid))
		return -EPERM;

	/* Make sure a caller can chmod. */
	if (ia_valid & ATTR_MODE) {
		if (!inode_owner_or_capable(mnt_userns, inode))
			return -EPERM;
		/* Also check the setgid bit! */
		if (!in_group_p((ia_valid & ATTR_GID) ? attr->ia_gid :
				i_gid_into_mnt(mnt_userns, inode)) &&
		    !capable_wrt_inode_uidgid(mnt_userns, inode, CAP_FSETID))
			attr->ia_mode &= ~S_ISGID;
	}

	/* Check for setting the inode time. */
	if (ia_valid & (ATTR_MTIME_SET | ATTR_ATIME_SET | ATTR_TIMES_SET)) {
		if (!inode_owner_or_capable(mnt_userns, inode))
			return -EPERM;
	}
#endif /* __KERNEL__ */

kill_priv:
	/* User has permission for the change */
	if (ia_valid & ATTR_KILL_PRIV) {
#ifdef __KERNEL__
		int error;

		error = security_inode_killpriv(mnt_userns, dentry);
		if (error)
			return error;
#endif
	}

	return 0;
}

int inode_newsize_ok(const struct inode *inode, loff_t offset)
{
	if (inode->i_size < offset) {
#ifdef __KERNEL__
		unsigned long limit;

		limit = rlimit(RLIMIT_FSIZE);
		if (limit != RLIM_INFINITY && offset > limit)
			goto out_sig;
#endif
		if (offset > inode->i_sb->s_maxbytes)
			goto out_big;
	}

	return 0;
#ifdef __KERNEL__
out_sig:
	send_sig(SIGXFSZ, current, 0);
#endif
out_big:
	return -EFBIG;
}

void setattr_copy(struct user_namespace *mnt_userns, struct inode *inode,
		  const struct iattr *attr)
{
	unsigned int ia_valid = attr->ia_valid;

	if (ia_valid & ATTR_UID)
		inode->i_uid = attr->ia_uid;
	if (ia_valid & ATTR_GID)
		inode->i_gid = attr->ia_gid;
	/* tux3 has nanosecond granularity */
	if (ia_valid & ATTR_ATIME)
		inode->i_atime = attr->ia_atime;
	if (ia_valid & ATTR_MTIME)
		inode->i_mtime = attr->ia_mtime;
	if (ia_valid & ATTR_CTIME)
		inode->i_ctime = attr->ia_ctime;
	if (ia_valid & ATTR_MODE) {
		umode_t mode = attr->ia_mode;
#ifdef __KERNEL__
		if (!in_group_p(inode->i_gid) &&
		    !capable_wrt_inode_uidgid(inode, CAP_FSETID))
			mode &= ~S_ISGID;
#endif
		inode->i_mode = mode;
	}
}

/*
 * inode stuff
 */

void inc_nlink(struct inode *inode)
{
	inode->i_nlink++;
}

void drop_nlink(struct inode *inode)
{
	BUG_ON(inode->i_nlink == 0);
	inode->i_nlink--;
}

void clear_nlink(struct inode *inode)
{
	inode->i_nlink = 0;
}

void set_nlink(struct inode *inode, unsigned int nlink)
{
	if (!nlink)
		clear_nlink(inode);
	else
		inode->i_nlink = nlink;
}

void inode_nohighmem(struct inode *inode)
{
	mapping_set_gfp_mask(mapping(inode), GFP_USER);
}

/*
 * dcache stuff
 */

void d_instantiate(struct dentry *dentry, struct inode *inode)
{
	dentry->d_inode = inode;
}

struct dentry *d_splice_alias(struct inode *inode, struct dentry *dentry)
{
	if (IS_ERR(inode))
		return ERR_CAST(inode);
	d_instantiate(dentry, inode);
	return NULL;
}

/*
 * truncate stuff
 */

void truncate_pagecache(struct inode *inode, loff_t newsize)
{
	truncate_inode_pages(mapping(inode), newsize);
}

void truncate_setsize(struct inode *inode, loff_t newsize)
{
	loff_t oldsize = inode->i_size;

	inode->i_size = newsize;
	if (newsize > oldsize)
		pagecache_isize_extended(inode, oldsize, newsize);
	if (newsize < oldsize)
		truncate_pagecache(inode, newsize);
}

/**
 * timestamp_truncate - Truncate timespec to a granularity
 * @t: Timespec
 * @inode: inode being updated
 *
 * Truncate a timespec to the granularity supported by the fs
 * containing the inode. Always rounds down. gran must
 * not be 0 nor greater than a second (NSEC_PER_SEC, or 10^9 ns).
 */
struct timespec64 timestamp_truncate(struct timespec64 t, struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	unsigned int gran = sb->s_time_gran;

	t.tv_sec = clamp(t.tv_sec, sb->s_time_min, sb->s_time_max);
	if (unlikely(t.tv_sec == sb->s_time_max || t.tv_sec == sb->s_time_min))
		t.tv_nsec = 0;

	/* Avoid division in the common cases 1 ns and 1 s. */
	if (gran == 1)
		; /* nothing */
	else if (gran == NSEC_PER_SEC)
		t.tv_nsec = 0;
	else if (gran > 1 && gran < NSEC_PER_SEC)
		t.tv_nsec -= t.tv_nsec % gran;
	else
		WARN(1, "invalid file time granularity: %u", gran);
	return t;
}

struct timespec64 current_time(struct inode *inode)
{
	struct timespec now;
	struct timespec64 ts_now;
	clock_gettime(CLOCK_REALTIME_COARSE, &now);
	ts_now = (struct timespec64){
		.tv_sec = now.tv_sec,
		.tv_nsec = now.tv_nsec,
	};
	return timestamp_truncate(ts_now, inode);
}
