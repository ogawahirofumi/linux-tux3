#ifndef LIBKLIB_FS_H
#define LIBKLIB_FS_H

/* depending on tux3 */

#include <libklib/lockdebug.h>
#include <libklib/uidgid.h>
#include <libklib/blk_types.h>

/* These sb flags are internal to the kernel */
#define MS_ACTIVE	(1<<30)

struct nameidata {
};

typedef struct {
	int	val[2];
} __kernel_fsid_t;

struct kstatfs {
	long f_type;
	long f_bsize;
	u64 f_blocks;
	u64 f_bfree;
	u64 f_bavail;
	u64 f_files;
	u64 f_ffree;
	__kernel_fsid_t f_fsid;
	long f_namelen;
	long f_frsize;
	long f_flags;
	long f_spare[4];
};

/*
 * Attribute flags.  These should be or-ed together to figure out what
 * has been changed!
 */
#define ATTR_MODE	(1 << 0)
#define ATTR_UID	(1 << 1)
#define ATTR_GID	(1 << 2)
#define ATTR_SIZE	(1 << 3)
#define ATTR_ATIME	(1 << 4)
#define ATTR_MTIME	(1 << 5)
#define ATTR_CTIME	(1 << 6)
#define ATTR_ATIME_SET	(1 << 7)
#define ATTR_MTIME_SET	(1 << 8)
#define ATTR_FORCE	(1 << 9) /* Not a change, but a change it */
#define ATTR_ATTR_FLAG	(1 << 10)
#define ATTR_KILL_SUID	(1 << 11)
#define ATTR_KILL_SGID	(1 << 12)
#define ATTR_FILE	(1 << 13)
#define ATTR_KILL_PRIV	(1 << 14)
#define ATTR_OPEN	(1 << 15) /* Truncating from open(O_TRUNC) */
#define ATTR_TIMES_SET	(1 << 16)

/*
 * This is the Inode Attributes structure, used for notify_change().  It
 * uses the above definitions as flags, to know which values have changed.
 * Also, in this manner, a Filesystem can look at only the values it cares
 * about.  Basically, these are the attributes that the VFS layer can
 * request to change from the FS layer.
 *
 * Derek Atkins <warlord@MIT.EDU> 94-10-20
 */
struct iattr {
	unsigned int	ia_valid;
	umode_t		ia_mode;
	kuid_t		ia_uid;
	kgid_t		ia_gid;
	loff_t		ia_size;
	struct timespec	ia_atime;
	struct timespec	ia_mtime;
	struct timespec	ia_ctime;
#ifdef __KERNEL__
	/*
	 * Not an attribute, but an auxiliary info for filesystems wanting to
	 * implement an ftruncate() like method.  NOTE: filesystem should
	 * check for (ia_valid & ATTR_FILE), and not for (ia_file != NULL).
	 */
	struct file	*ia_file;
#endif
};

int inode_change_ok(const struct inode *inode, struct iattr *attr);
int inode_newsize_ok(const struct inode *inode, loff_t offset);
void setattr_copy(struct inode *inode, const struct iattr *attr);

/* Generic inode */
struct inode {
	struct super_block	*i_sb;

	struct mutex		i_mutex;
	unsigned long		i_state;
	atomic_t		i_count;

	umode_t			i_mode;
	kuid_t			i_uid;
	kgid_t			i_gid;
	unsigned int		i_nlink;
	dev_t			i_rdev;
	loff_t			i_size;
	struct timespec		i_atime;
	struct timespec		i_mtime;
	struct timespec		i_ctime;
	spinlock_t		i_lock;
	u64			i_version;

	map_t			*map;
	struct hlist_node	i_hash;
	union {
		struct rcu_head		i_rcu;
	};
};

/*
 * Helper functions so that in most cases filesystems will
 * not need to deal directly with kuid_t and kgid_t and can
 * instead deal with the raw numeric values that are stored
 * in the filesystem.
 */
static inline uid_t i_uid_read(const struct inode *inode)
{
	return from_kuid(&init_user_ns, inode->i_uid);
}

static inline gid_t i_gid_read(const struct inode *inode)
{
	return from_kgid(&init_user_ns, inode->i_gid);
}

static inline void i_uid_write(struct inode *inode, uid_t uid)
{
	inode->i_uid = make_kuid(&init_user_ns, uid);
}

static inline void i_gid_write(struct inode *inode, gid_t gid)
{
	inode->i_gid = make_kgid(&init_user_ns, gid);
}

/*
 * dentry stuff
 */

struct qstr {
	/* unsigned int hash; */
	unsigned int len;
	const unsigned char *name;
};

struct dentry {
	struct qstr d_name;
	struct inode *d_inode;
	struct super_block *d_sb;
};

void d_instantiate(struct dentry *dentry, struct inode *inode);
struct dentry *d_splice_alias(struct inode *inode, struct dentry *dentry);

/*
 * fs stuff
 */

#define READ_SYNC	REQ_SYNC
#define WRITE_SYNC	(REQ_SYNC | REQ_NOIDLE)
#define WRITE_ODIRECT	(REQ_SYNC)
#define WRITE_FLUSH	(REQ_SYNC | REQ_NOIDLE | REQ_PREFLUSH)
#define WRITE_FUA	(REQ_SYNC | REQ_NOIDLE | REQ_FUA)
#define WRITE_FLUSH_FUA	(REQ_SYNC | REQ_NOIDLE | REQ_PREFLUSH | REQ_FUA)

static inline bool op_is_write(unsigned int op)
{
	return op == REQ_OP_READ ? false : true;
}

/* File handle */
struct file {
	struct inode	*f_inode;
	u64		f_version;
	loff_t		f_pos;
};

#define MAX_LFS_FILESIZE	((loff_t)LLONG_MAX)

static inline struct inode *file_inode(struct file *f)
{
	return f->f_inode;
}

/*
 * File types
 *
 * NOTE! These match bits 12..15 of stat.st_mode
 * (ie "(i_mode >> 12) & 15").
 */
#define DT_UNKNOWN	0
#define DT_FIFO		1
#define DT_CHR		2
#define DT_DIR		4
#define DT_BLK		6
#define DT_REG		8
#define DT_LNK		10
#define DT_SOCK		12
#define DT_WHT		14

struct dir_context;
typedef int (*filldir_t)(struct dir_context *, const char *, int, loff_t, u64,
			 unsigned);
struct dir_context {
	const filldir_t actor;
	loff_t pos;
};

static inline bool dir_emit(struct dir_context *ctx,
			    const char *name, int namelen,
			    u64 ino, unsigned type)
{
	return ctx->actor(ctx, name, namelen, ctx->pos, ino, type) == 0;
}
static inline bool dir_relax(struct inode *inode)
{
	return true;
}

static inline void inode_dio_wait(struct inode *inode)
{
}

void inc_nlink(struct inode *inode);
void drop_nlink(struct inode *inode);
void clear_nlink(struct inode *inode);
void set_nlink(struct inode *inode, unsigned int nlink);

void mark_inode_dirty(struct inode *inode);
static inline void inode_inc_link_count(struct inode *inode)
{
	inc_nlink(inode);
	mark_inode_dirty(inode);
}

static inline void inode_dec_link_count(struct inode *inode)
{
	drop_nlink(inode);
	mark_inode_dirty(inode);
}

static inline void inode_writeback_touch(struct inode *inode)
{
}

static inline void inode_writeback_done(struct inode *inode)
{
}

#endif /* !LIBKLIB_FS_H */
