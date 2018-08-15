#include <tux3user.h>

/* depending on tux3 */

/*
 * Attribute stuff
 */

int setattr_prepare(struct dentry *dentry, struct iattr *attr)
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
	if ((ia_valid & ATTR_UID) &&
	    (!uid_eq(current_fsuid(), inode->i_uid) ||
	     !uid_eq(attr->ia_uid, inode->i_uid)) &&
	    !capable_wrt_inode_uidgid(inode, CAP_CHOWN))
		return -EPERM;

	/* Make sure caller can chgrp. */
	if ((ia_valid & ATTR_GID) &&
	    (!uid_eq(current_fsuid(), inode->i_uid) ||
	    (!in_group_p(attr->ia_gid) && !gid_eq(attr->ia_gid, inode->i_gid))) &&
	    !capable_wrt_inode_uidgid(inode, CAP_CHOWN))
		return -EPERM;

	/* Make sure a caller can chmod. */
	if (ia_valid & ATTR_MODE) {
		if (!inode_owner_or_capable(inode))
			return -EPERM;
		/* Also check the setgid bit! */
		if (!in_group_p((ia_valid & ATTR_GID) ? attr->ia_gid :
				inode->i_gid) &&
		    !capable_wrt_inode_uidgid(inode, CAP_FSETID))
			attr->ia_mode &= ~S_ISGID;
	}

	/* Check for setting the inode time. */
	if (ia_valid & (ATTR_MTIME_SET | ATTR_ATIME_SET | ATTR_TIMES_SET)) {
		if (!inode_owner_or_capable(inode))
			return -EPERM;
	}
#endif /* __KERNEL__ */

kill_priv:
	/* User has permission for the change */
	if (ia_valid & ATTR_KILL_PRIV) {
#ifdef __KERNEL__
		int error;

		error = security_inode_killpriv(dentry);
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

void setattr_copy(struct inode *inode, const struct iattr *attr)
{
	unsigned int ia_valid = attr->ia_valid;

	if (ia_valid & ATTR_UID)
		inode->i_uid = attr->ia_uid;
	if (ia_valid & ATTR_GID)
		inode->i_gid = attr->ia_gid;
	/* tux3 has nanosecond granularity */
	if (ia_valid & ATTR_ATIME)
		inode->i_atime = timespec64_trunc(attr->ia_atime,
						  inode->i_sb->s_time_gran);
	if (ia_valid & ATTR_MTIME)
		inode->i_mtime = timespec64_trunc(attr->ia_mtime,
						  inode->i_sb->s_time_gran);
	if (ia_valid & ATTR_CTIME)
		inode->i_ctime = timespec64_trunc(attr->ia_ctime,
						  inode->i_sb->s_time_gran);
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
 * timespec64_trunc - Truncate timespec64 to a granularity
 * @t: Timespec64
 * @gran: Granularity in ns.
 *
 * Truncate a timespec64 to a granularity. Always rounds down. gran must
 * not be 0 nor greater than a second (NSEC_PER_SEC, or 10^9 ns).
 */
struct timespec64 timespec64_trunc(struct timespec64 t, unsigned gran)
{
	/* Avoid division in the common cases 1 ns and 1 s. */
	if (gran == 1) {
		/* nothing */
	} else if (gran == NSEC_PER_SEC) {
		t.tv_nsec = 0;
	} else if (gran > 1 && gran < NSEC_PER_SEC) {
		t.tv_nsec -= t.tv_nsec % gran;
	} else {
		WARN(1, "illegal file time granularity: %u", gran);
	}
	return t;
}

struct timespec64 current_time(struct inode *inode)
{
	struct timeval now;
	struct timespec64 ts_now;
	gettimeofday(&now, NULL);
	ts_now = (struct timespec64){
		.tv_sec = now.tv_sec,
		.tv_nsec = now.tv_usec * 1000
	};
	return timespec64_trunc(ts_now, inode->i_sb->s_time_gran);
}
