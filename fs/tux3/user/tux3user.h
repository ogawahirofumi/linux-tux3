#ifndef TUX3_USER_H
#define TUX3_USER_H

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <limits.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

/* libklib/libklib.h is before others, because defines "inline" etc. */
#include "libklib/libklib.h"
#include "buffer.h"		/* include early for map_t */

#include "libklib/lockdebug.h"
#include "libklib/atomic.h"
#include "libklib/refcount.h"
#include "libklib/mm.h"
#include "libklib/slab.h"
#include "libklib/fs.h"
#include "libklib/fs_types.h"
#include "libklib/parser.h"
#include "libklib/iversion.h"

#include "trace.h"
#include "current_task.h"
#include "options.h"
#include "writeback.h"

#ifndef XATTR_CREATE
#define XATTR_CREATE  0x1       /* set value, fail if attr already exists */
#define XATTR_REPLACE 0x2       /* set value, fail if attr does not exist */
#endif

static inline struct inode *buffer_inode(struct buffer_head *buffer)
{
	return buffer->map->inode;
}

struct super_block {
	struct dev *dev;		/* userspace block device */
	loff_t s_maxbytes;		/* maximum file size */
	unsigned long s_flags;
	unsigned long s_magic;
	unsigned int s_max_links;	/* maximum link counts */

	/* Granularity of c/m/atime in ns (cannot be worse than a second) */
	u32 s_time_gran;
	/* Time limits for c/m/atime in seconds */
	time64_t s_time_min;
	time64_t s_time_max;
};

static inline struct sb *tux_sb(struct super_block *sb);
static inline struct super_block *vfs_sb(struct sb *sb);
static inline map_t *mapping(struct inode *inode);
static inline struct dev *sb_dev(struct sb *sb);

#include "../tux3.h"

static inline struct sb *tux_sb(struct super_block *sb)
{
	return container_of(sb, struct sb, vfs_sb);
}

static inline struct super_block *vfs_sb(struct sb *sb)
{
	return &sb->vfs_sb;
}

static inline map_t *mapping(struct inode *inode)
{
	return inode->map;
}

static inline struct dev *sb_dev(struct sb *sb)
{
	return vfs_sb(sb)->dev;
}

#define READAHEAD_BLOCKS (1 << 6)

#define INIT_DISKSB(_bits, _blocks) (struct disksuper){		\
	.magic		= TUX3_MAGIC,				\
	.birthdate	= 0,					\
	.flags		= 0,					\
	.blockbits	= cpu_to_be16(_bits),			\
	.volblocks	= cpu_to_be64(_blocks),			\
								\
	.iroot		= cpu_to_be64(pack_root(&no_root)),	\
	.oroot		= cpu_to_be64(pack_root(&no_root)),	\
	/* Set "reserved inodes" as used inode */		\
	.usedinodes	= cpu_to_be64(TUX_NORMAL_INO),		\
	.nextblock	= 0,					\
	.atomdictsize	= 0,					\
	.freeatom	= 0,					\
	.atomgen	= cpu_to_be32(1),			\
	.logchain	= 0,					\
	.logcount	= 0,					\
}

#define rapid_sb(x)	(&(struct sb){		\
	.mopt = tux3_default_mopt,		\
	.vfs_sb = {				\
		.dev = x,			\
		.s_time_gran = TUX3_TIME_GRAN,	\
		.s_time_min = TUX3_TIME_MIN,	\
		.s_time_max = TUX3_TIME_MAX,	\
	},					\
})

/* struct file initializer */
#define FILE_INIT(i, p)		{		\
	.f_inode	= i,			\
	.f_version	= 0,			\
	.f_pos		= p,			\
}

/* commit.c */
int force_unify(struct sb *sb);
int force_delta(struct sb *sb);
int sync_super(struct sb *sb);

/* dir.c */
void tux_dump_entries(struct buffer_head *buffer);

/* filemap.c */
int tuxread(struct file *file, void *data, unsigned len);
int tuxwrite(struct file *file, const void *data, unsigned len);
void tuxseek(struct file *file, loff_t pos);
int page_symlink(struct inode *inode, const char *symname, int len);
int page_readlink(struct inode *inode, void *buf, unsigned size);

/* inode.c */
void inode_leak_check(void);
void remove_inode_hash(struct inode *inode);
void inode_init_once(struct inode *inode);
void unlock_new_inode(struct inode *inode);
void discard_new_inode(struct inode *inode);
void __iget(struct inode *inode);
void ihold(struct inode *inode);
loff_t i_size_read(const struct inode *inode);
void i_size_write(struct inode *inode, loff_t i_size);
void iput(struct inode *inode);
int __tuxtruncate(struct inode *inode, loff_t size);
int tuxtruncate(struct inode *inode, loff_t size);
struct inode *rapid_new_inode(struct sb *sb, blockio_t *io, umode_t mode);
void rapid_free_inode(struct inode *inode);

/* namei.c */
struct inode *tuxopen(struct inode *dir, const char *name, unsigned len);
struct inode *__tuxmknod(struct inode *dir, const char *name, unsigned len,
			 struct tux_iattr *iattr);
struct inode *tuxmknod(struct inode *dir, const char *name, unsigned len,
		       struct tux_iattr *iattr);
struct inode *tuxcreate(struct inode *dir, const char *name, unsigned len,
			struct tux_iattr *iattr);
struct inode *__tuxlink(struct inode *src_inode, struct inode *dir,
			const char *dstname, unsigned dstlen);
int tuxlink(struct inode *dir, const char *srcname, unsigned srclen,
	    const char *dstname, unsigned dstlen);
int tuxreadlink(struct inode *dir, const char *name, unsigned len,
		void *buf, unsigned bufsize);
struct inode *__tuxsymlink(struct inode *dir, const char *name, unsigned len,
			   struct tux_iattr *iattr, const char *symname);
int tuxsymlink(struct inode *dir, const char *name, unsigned len,
	       struct tux_iattr *iattr, const char *symname);
int tuxunlink(struct inode *dir, const char *name, unsigned len);
int tuxrmdir(struct inode *dir, const char *name, unsigned len);
int tuxrename(struct inode *old_dir, const char *old_name, unsigned old_len,
	      struct inode *new_dir, const char *new_name, unsigned new_len,
	      unsigned int flags);

/* super.c */
struct inode *__alloc_inode(struct super_block *sb);
void __destroy_inode_nocheck(struct inode *inode);
void __destroy_inode(struct inode *inode);
void put_super(struct sb *sb);
int setup_sb(struct sb *sb, struct disksuper *super);
struct replay *__load_fs(struct sb *sb);
int load_fs(struct sb *sb, int apply_orphan);
int __mkfs_tux3(struct sb *sb);
int mkfs_tux3(struct sb *sb);
int setup_mount_options(struct sb *sb, void *data);
ssize_t get_mount_options(struct sb *sb, char *buf, size_t size, int all);
int tux3_get_kstatfs(struct sb *sb, struct kstatfs *kstatfs);
int tux3_init_mem(unsigned poolsize, int debug);
void tux3_exit_mem(void);

/* utility.c */
void stacktrace(void);
int devio_vec(enum req_opf req_opf, unsigned int req_flags, struct dev *dev,
	      loff_t offset, struct iovec *iov, unsigned iovcnt);
int devio_sync(enum req_opf req_opf, unsigned int req_flags,
	       struct dev *dev, loff_t offset, void *data,
	       unsigned len);
int blockio_sync(enum req_opf req_opf, unsigned int req_flags, struct sb *sb,
		 struct buffer_head *buffer, block_t block);
int blockio_vec(struct bufvec *bufvec, block_t block, unsigned count);

#define tux3_msg(sb, fmt, ...)						\
	__tux3_msg(sb, "", "", fmt "\n", ##__VA_ARGS__)
#define __tux3_err(sb, func, line, fmt, ...)				\
	__tux3_msg(sb, "", "",						\
		   "Error: %s:%d: " fmt "\n", func, line, ##__VA_ARGS__)
#define tux3_err(sb, fmt, ...)						\
	__tux3_err(sb, __func__, __LINE__, fmt, ##__VA_ARGS__)
#define tux3_warn(sb, fmt, ...)					\
	__tux3_msg(sb, "", "", "Warning: " fmt "\n", ##__VA_ARGS__)

#define strerror_exit(ret, err, fmt, ...) do {				\
	if (err)							\
		tux3_err(NULL, fmt ": %s", ##__VA_ARGS__, strerror(err)); \
	else								\
		tux3_err(NULL, fmt, ##__VA_ARGS__);			\
	exit(ret);							\
} while (0)

#define error_exit(fmt, ...) do {					\
	strerror_exit(1, 0, fmt, ##__VA_ARGS__);			\
} while (0)

#endif /* !TUX3_USER_H */
