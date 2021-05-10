#ifndef TUX3_H
#define TUX3_H

#ifdef __KERNEL__
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <linux/writeback.h>
#include <linux/backing-dev.h>
#include <linux/fs.h>
#include <linux/fs_types.h>
#include <linux/memcontrol.h>	/* for lock_page_memcg */
#include <linux/buffer_head.h>
#include <linux/bio.h>
#include <linux/blkdev.h>	/* for struct blk_plug */
#include <linux/mutex.h>
#include <linux/magic.h>
#include <linux/slab.h>
#include <linux/xattr.h>
#include <linux/list_sort.h>
#include <linux/iversion.h>
#include <asm/unaligned.h>

#include "trace.h"
#include "buffer.h"
#endif /* __KERNEL__ */

#include "link.h"

#define EFSCORRUPTED	EUCLEAN		/* Filesystem is corrupted */

#define fieldtype(compound, field) typeof(((compound *)NULL)->field)
#define vecset(d, v, n) memset((d), (v), (n) * sizeof(*(d)))
#define veccopy(d, s, n) memcpy((d), (s), (n) * sizeof(*(d)))
#define vecmove(d, s, n) memmove((d), (s), (n) * sizeof(*(d)))

typedef u64 inum_t;
typedef u64 tuxkey_t;

static inline void *encode16(void *at, unsigned val)
{
	put_unaligned_be16(val, at);
	return at + sizeof(u16);
}

static inline void *encode32(void *at, unsigned val)
{
	put_unaligned_be32(val, at);
	return at + sizeof(u32);
}

static inline void *encode64(void *at, u64 val)
{
	put_unaligned_be64(val, at);
	return at + sizeof(u64);
}

static inline void *encode48(void *at, u64 val)
{
	at = encode16(at, val >> 32);
	return encode32(at, val);
}

static inline void *decode16(void *at, unsigned *val)
{
	*val = get_unaligned_be16(at);
	return at + sizeof(u16);
}

static inline void *decode32(void *at, unsigned *val)
{
	*val = get_unaligned_be32(at);
	return at + sizeof(u32);
}

static inline void *decode64(void *at, u64 *val)
{
	*val = get_unaligned_be64(at);
	return at + sizeof(u64);
}

static inline void *decode48(void *at, u64 *val)
{
	unsigned part1, part2;
	at = decode16(at, &part1);
	at = decode32(at, &part2);
	*val = (u64)part1 << 32 | part2;
	return at;
}

/* Tux3 disk format */

/*
 * TUX3_LABEL includes the date of the last incompatible disk format change
 * NOTE: Always update this history for each incompatible change!
 *
 * Disk Format Revision History
 *
 * 2008-08-06: Beginning of time
 * 2008-09-06: Actual checking starts
 * 2008-12-12: Atom dictionary size in disksuper instead of atable->i_size
 * 2009-02-28: Attributes renumbered, rdev added
 * 2009-03-10: Alignment fix of disksuper
 * 2012-02-16: Update for atomic commit
 * 2012-07-02: Use timestamp 32.32 fixed point. Increase log_balloc size.
 * 2012-12-20: Add ->usedinodes
 * 2014-03-27: Change internal inum numbers. Change btree->root.depth.
 * 2014-05-06: Change timestamp format to nanosecond.
 * 2015-03-14: Add parent_inum
 * 2015-05-23: Use new ileaf format
 * 2015-06-01: Define IATTR defaults, use 16bits for i_mode
 */
#define TUX3_MAGIC		{ 't', 'u', 'x', '3', 0x20, 0x15, 0x06, 0x01 }
#define TUX3_MAGIC_STR					\
	((typeof(((struct disksuper *)0)->magic))TUX3_MAGIC)

/*
 * Magic numbers should not conflict with well known magic number,
 * and probably should not use the ascii magic number.
 * (e.g. \0177ELF is bad)
 */
#define TUX3_MAGIC_LOG		0x10ad
#define TUX3_MAGIC_BNODE	0xb4de
#define TUX3_MAGIC_DLEAF	0xda7a
#define TUX3_MAGIC_ILEAF	0x90de
#define TUX3_MAGIC_OLEAF	0x6eaf

/* Number of available inum ("0" - "((1 << 48) - 1)") */
#define MAX_INODES_BITS		48
#define MAX_INODES		((u64)1 << MAX_INODES_BITS)
/* Maximum number of block address ("0" - "((1 << 48) - 1)") */
#define MAX_BLOCKS_BITS		48
#define MAX_BLOCKS		((block_t)1 << MAX_BLOCKS_BITS)

#define SB_LOC			(1 << 12)
#define SB_LEN			(1 << 12)	/* this is maximum blocksize */

#define MAX_TUXKEY		(((tuxkey_t)1 << 48) - 1)
#define TUXKEY_LIMIT		(MAX_TUXKEY + 1)

/* Special inode numbers */
#define TUX_INVALID_INO		0	/* FIXME: just for debugging */
#define TUX_BITMAP_INO		1
#define TUX_COUNTMAP_INO	2	/* Block group free count map */
#define TUX_VTABLE_INO		3
#define TUX_ATABLE_INO		4
#define TUX_VOLMAP_INO		61	/* This doesn't have entry in ileaf */
#define TUX_LOGMAP_INO		62	/* This is volmap for log blocks */
#define TUX_ROOTDIR_INO		63
#define TUX_NORMAL_INO		64	/* until this ino, reserved ino */

/* Timestamp granularity */
#define TUX3_TIME_GRAN		1	/* nanoseconds */
/* Timestamp min/max in seconds
 * FIXME: s_time_{min,max} can't use nsecs, so tux3 can't specify real limit */
#define TUX3_TIME_MIN		(S64_MIN / NSEC_PER_SEC)
#define TUX3_TIME_MAX		(S64_MAX / NSEC_PER_SEC)

struct disksuper {
	/* Update magic on any incompatible format change */
	char magic[8];		/* Contains TUX3_LABEL magic string */
	__be64 birthdate;	/* Volume creation date */
	__be64 flags;		/* Need to assign some flags */
	__be16 blockbits;	/* Shift to get volume block size */
	__be16 unused[3];	/* Padding for alignment */
	__be64 volblocks;	/* Volume size */

	/* The rest should be moved to a "metablock" that is updated frequently */
	__be64 iroot;		/* Root of the itree */
	__be64 oroot;		/* Root of the otree */
	__be64 usedinodes;	/* Number of using inode numbers.  (Instead of
				 * free inode numbers).  With this, we can
				 * change the maximum inodes without changing
				 * usedinodes on disk.
				 */
	__be64 nextblock;	/* Get rid of this when we have a real allocation policy */
	__be64 atomdictsize;	/*
				 * Size of the atom dictionary instead of i_size
				 * FIXME: we are better to remove this somehow
				 */
	__be32 freeatom;	/* Beginning of persistent free atom list in atable */
	__be32 atomgen;		/* Next atom number if there are no free atoms */
	__be64 logchain;	/* Most recent delta commit block pointer */
	__be32 logcount;	/* Count of log blocks in the current log chain */
	__be32 unused2[1];
};

enum { MAX_DIRECT_COUNT = SHRT_MAX };

/* FIXME: maybe better to remove struct root to reduce holes in structure? */
struct root {
	unsigned short direct; /* block/depth is an extent instead of btree */
#if 0
	/* gcc-4.4 seems to not support to use initializer for unnamed union */
	union {
		short count;	/* Number of blocks in direct extent */
		short depth;	/* Btree levels include leaf level */
	};
#else
	short depth;		/* direct=0: Btree levels include leaf level.
				 * direct=1: Blocks in direct extent */
#endif
	block_t block;	/* Disk location of btree root */
};

struct btree {
	struct rw_semaphore lock;
	struct sb *sb;		/* Convenience to reduce parameter list size */
	struct btree_ops *ops;	/* Generic btree low level operations */
	struct root root;	/* Cached description of btree root */
	u16 entries_per_leaf;	/* Used in btree leaf splitting */
};

/* Define layout of btree root on disk, endian conversion is elsewhere. */

static inline u64 pack_root(struct root *root)
{
	return (u64)root->direct << 63 | (u64)root->depth << 48 | root->block;
}

static inline struct root unpack_root(u64 v)
{
	return (struct root){
		.direct = v >> 63,
		.depth = (v >> 48) & 0x7fff,
		.block = v & (-1ULL >> 16),
	};
}

/* Path cursor for btree traversal */

struct cursor {
	struct btree *btree;
#define CURSOR_DEBUG
#ifdef CURSOR_DEBUG
#define FREE_BUFFER	((void *)0xdbc06505)
#define FREE_NEXT	0xdbc06507
	int maxlevel;
#endif
	int level;
	struct path_level {
		struct buffer_head *buffer;
#define CURSOR_LEAF_LEVEL	-2
		int next;
	} path[];
};

/* Logging  */

struct logpos {
	unsigned next;			/* Index of next log block in log map */
	struct buffer_head *buf;	/* Cached log block */
	unsigned char *pos;		/* Where to emit next log entry */
	unsigned char *top;		/* End of logbuf */
};

struct logblock {
	__be16 magic;		/* Magic number */
	__be16 bytes;		/* Total data bytes on this block */
	u32 unused;		/* padding */
	__be64 logchain;	/* Block number to previous logblock */
	unsigned char data[];	/* Log data */
};

enum {
	LOG_BALLOC = 0x33,	/* Log of block allocation */
	LOG_BFREE,		/* Log of freeing block after delta */
	LOG_BFREE_ON_UNIFY,	/* Log of freeing block after unify */
	LOG_BFREE_RELOG,	/* LOG_BFREE, but re-log of free after unify */
	LOG_LEAF_REDIRECT,	/* Log of leaf redirect */
	LOG_LEAF_FREE,		/* Log of freeing leaf */
	LOG_BNODE_REDIRECT,	/* Log of bnode redirect */
	LOG_BNODE_ROOT,		/* Log of new bnode root allocation */
	LOG_BNODE_SPLIT,	/* Log of spliting bnode to new bnode */
	LOG_BNODE_ADD,		/* Log of adding bnode index */
	LOG_BNODE_UPDATE,	/* Log of bnode index ->block update */
	LOG_BNODE_MERGE,	/* Log of merging 2 bnodes */
	LOG_BNODE_DEL,		/* Log of deleting bnode index */
	LOG_BNODE_ADJUST,	/* Log of bnode index ->key adjust */
	LOG_BNODE_FREE,		/* Log of freeing bnode */
	LOG_ORPHAN_ADD,		/* Log of adding orphan inode */
	LOG_ORPHAN_DEL,		/* Log of deleting orphan inode */
	LOG_FREEBLOCKS,		/* Log of freeblocks in bitmap on unify */
	LOG_UNIFY,		/* Log of marking unify */
	LOG_DELTA,		/* just for debugging */
	LOG_TYPES
};

struct stash {
	struct flink_head head;
	u64 *pos, *top;
};

/* ENOSPC management */
struct nospc_data {
	block_t free;		/* Available freeblocks for delta (backend)*/
	block_t reserve;	/* Reserved blocks for delta (backend) */
	atomic64_t budget;	/* Usable blocks for delta */
	atomic64_t balance;	/* Current remaining blocks */
};
/*
 * Cost of operations. (Exclude cost in unify stage.)
 */
/* inode deletion and orphan entry add. FIXME: no need oleaf? */
#define TUX3_COST_ORPHAN	2	/* ileaf + oleaf */
/* orphan entry deletion. FIXME: no need oleaf? */
#define TUX3_COST_ORPHAN_DEL	1	/* oleaf */

struct defree {
	block_t blocks;
	struct stash stash;
};

/* Orphaned inode/inum management */
struct orphan_head {
	long long count;	/* Orphans in log-records + otree */
	long long count_del;	/* Inode was delete, but inum is in otree */

	struct list_head add_head;	/* defered orphan inode add list */
	struct list_head del_head;	/* defered orphan inode del list */
};

#ifndef TUX3_FLUSHER_SYNC
/* For each delta + 1 remaining until bdi call complete() + safety */
#define TUX3_MAX_WB_WORK	(TUX3_MAX_DELTA + 2)
/* Work item for writeback flusher */
struct tux3_wb_work {
	struct wb_writeback_work work;
	int flusher_is_waiting;
	unsigned delta;
};
#endif

/* Refcount for delta */
struct delta_ref {
	refcount_t refcount;
	unsigned delta;
	struct completion waitref_done;
};

/* Per-delta data structure for sb */
struct sb_delta_dirty {
	struct list_head dirty_inodes;	/* dirty inodes list */
	atomic64_t nospc_charged;	/* dirty block allocation upper bound
					 * for this delta */
};

/* Pin a block in cache and keep a pointer to it */
struct countmap_pin {
	struct buffer_head *buffer;
};

/* Mount options bits */
enum {
	TUX3_MOPT_BARRIER =	(1 << 0),
};

#define __TUX3_TEST_MOPT(m, f)	((m)->flags & f)
#define TUX3_TEST_MOPT(s, n)	__TUX3_TEST_MOPT(&(s)->mopt, TUX3_MOPT_##n)
#define __TUX3_SET_MOPT(m, f)	((m)->flags |= f)
#define TUX3_SET_MOPT(s, n)	__TUX3_SET_MOPT(&(s)->mopt, TUX3_MOPT_##n)
#define __TUX3_CLEAR_MOPT(m, f)	((m)->flags &= ~(f))
#define TUX3_CLEAR_MOPT(s, n)	__TUX3_CLEAR_MOPT(&(s)->mopt, TUX3_MOPT_##n)

struct tux3_mount_opt {
	unsigned int flags;
};

struct tux3_idefer_map;
/* Tux3-specific sb is a handle for the entire volume state */
struct sb {
	union {
		struct disksuper super;
		char thisbig[SB_LEN];
	};

	struct delta_ref __rcu *current_delta;	/* current delta */
	struct delta_ref delta_refs[TUX3_MAX_DELTA];
	unsigned unify;				/* log unify cycle */

#define TUX3_STATE_TRANSITION_BIT	0
	unsigned long backend_state;		/* delta state */

#ifdef TUX3_FLUSHER_SYNC
	struct rw_semaphore delta_lock;		/* delta transition exclusive */
#else
	struct tux3_wb_work wb_work[TUX3_MAX_WB_WORK];
#endif
	unsigned delta_waitref;			/* waiting refcount delta */
	unsigned delta_pending;			/* pending delta */
	unsigned delta_staging;			/* staging delta */
	unsigned delta_commit;			/* committed delta */
	unsigned delta_free;			/* free delta */
	/* wait queue for transition event */
	wait_queue_head_t delta_transition_wq;
	/* wait queue for commit event */
	wait_queue_head_t delta_commit_wq;

	struct tux3_mount_opt mopt;		/* mount options */

	struct btree itree;	/* Inode btree */
	struct btree otree;	/* Orphan btree */
	struct inode *volmap;	/* Volume metadata cache (like blockdev). */
	struct inode *bitmap;	/* allocation bitmap special file */
	struct inode *countmap;	/* block group free count map */
	struct inode *rootdir;	/* root directory special file */
	struct inode *vtable;	/* version table special file */
	struct inode *atable;	/* xattr atom special file */

	unsigned blocksize, blockbits, blockmask, groupbits;
	unsigned blocks_per_page, blocks_per_page_bits;

	block_t volblocks, volmask;

	block_t freeblocks;	/* Number of free blocks on backend. */
	block_t nextblock;
	u64 freeinodes;		/* Number of free inode numbers. This is
				 * including the deferred allocated inodes */
	inum_t nextinum;	/* FIXME: temporary hack to avoid to find
				 * same area in itree for free inum. */

	unsigned bnode_max_count; /* Maximum count of index in bnode */
	unsigned bnode_dict_size; /* Size for bnode internal dict */

	unsigned version;	/* Currently mounted volume version view */

	block_t atomref_base;	/* Index of atom refcount base */
	block_t unatom_base;	/* Index of unatom base */
	loff_t atomdictsize;	/* Atom dictionary size */
	unsigned freeatom;	/* Start of free atom list in atom table */
	unsigned atomgen;	/* Next atom number to allocate if no free atoms */

	/*
	 * For backend only
	 */
	struct inode *logmap;	/* Log block cache */
	struct logpos logpos;

	struct orphan_head orphan;	/* Orphaned inode management */

	struct defree defree;	/* defer extent frees until after delta */
	struct defree deunify;	/* defer extent frees until after unify */

	struct list_head unify_buffers; /* dirty metadata flushed at unify */
	struct list_head phase2_buffers; /* 2nd phase volmap buffers to clean */

	struct ioinfo *ioinfo;		/* helper for waiting I/O */

	/*
	 * For frontend and backend
	 */
	struct nospc_data nospc;	/* ENOSPC management */
	spinlock_t countmap_lock;
	struct countmap_pin countmap_pin;
	struct tux3_idefer_map *idefer_map;

	spinlock_t forked_buffers_lock;
	struct link forked_buffers;	/* forked buffers list */

	spinlock_t dirty_inodes_lock;	/* lock of dirty_inodes for frontend */
	/* Per-delta dirty data for sb */
	struct sb_delta_dirty s_ddc[TUX3_MAX_DELTA];
	struct buffer_head *last_dleaf;
#ifdef __KERNEL__
	struct super_block *vfs_sb;	/* Generic kernel superblock */
#else
	struct super_block vfs_sb;	/* Userland superblock */
#endif
};

/* Block segment (physical block extent) info */
#define BLOCK_SEG_HOLE		(1 << 0)
#define BLOCK_SEG_NEW		(1 << 1)

struct block_segment {
	block_t block;		/* Start of physical address */
	unsigned count;		/* Number of blocks */
	unsigned state;		/* State of this segment */
};

/*
 * Balloc flags
 */
/* Allow fewer allocation than requested */
#define BALLOC_PARTIAL		(1 << 0)

/* Just for debugging -1 */
#define TUX3_INVALID_IDATA		((umode_t)-1U)

/* Inode attributes data */
struct tux3_iattr_data {
	umode_t			i_mode;
	uid_t			i_uid;
	gid_t			i_gid;
	unsigned int		i_nlink;
	loff_t			i_size;
//	struct timespec64	i_atime;
	struct timespec64	i_mtime;
	struct timespec64	i_ctime;
	u64			i_version;
	/* inode type specific field */
	u64			generic;
};

/* Per-delta data structure for inode */
struct inode_delta_dirty {
	struct list_head dirty_buffers;	/* list for dirty buffers */
	struct list_head dirty_holes;	/* list for hole extents */
	struct list_head dirty_list;	/* link for dirty inode list */

	/* Forked data storage */
	/* FIXME: we don't need this for frontend delta. We would want
	 * to allocate dynamically */
	struct tux3_iattr_data idata;
};

struct xcache;
struct tux3_inode {
	struct btree btree;
	inum_t inum;			/* Inode number */
	unsigned long flags;		/* Inode flags */
	struct xcache *xcache;		/* Extended attribute cache */
	struct list_head orphan_list;	/* link for orphan inode list */

	/* FIXME: we can use RCU for hole_extents? */
	spinlock_t hole_extents_lock;	/* lock for hole_extents */
	struct list_head hole_extents;	/* hole extents list */

	struct rw_semaphore truncate_lock; /* lock for truncate and mmap */
	spinlock_t lock;		/* lock for inode metadata */

	/* inode type specific field */
	union {
		inum_t parent_inum;	/* parent inum for dir */
		u64 generic;
	};

	/* Per-delta dirty data for inode */
	unsigned state;			/* inode dirty state */
	struct inode_delta_dirty i_ddc[TUX3_MAX_DELTA];
#ifdef __KERNEL__
	int (*io)(struct bufvec *bufvec);
#endif
	/* Generic inode */
	struct inode vfs_inode;
};

static inline struct tux3_inode *tux_inode(struct inode *inode)
{
	return container_of(inode, struct tux3_inode, vfs_inode);
}

static inline struct inode *btree_inode(struct btree *btree)
{
	return &container_of(btree, struct tux3_inode, btree)->vfs_inode;
}
#ifdef __KERNEL__
static inline struct sb *tux_sb(struct super_block *sb)
{
	return sb->s_fs_info;
}

static inline struct super_block *vfs_sb(struct sb *sb)
{
	return sb->vfs_sb;
}

static inline struct address_space *mapping(struct inode *inode)
{
	return inode->i_mapping;
}

static inline struct block_device *sb_dev(struct sb *sb)
{
	return sb->vfs_sb->s_bdev;
}
#endif /* __KERNEL__ */

/* Get delta from free running counter */
static inline unsigned tux3_delta(unsigned delta)
{
	return delta & (TUX3_MAX_DELTA - 1);
}

/* Get per-delta dirty data control for sb */
static inline struct sb_delta_dirty *tux3_sb_ddc(struct sb *sb, unsigned delta)
{
	return &sb->s_ddc[tux3_delta(delta)];
}

/* Get per-delta dirty data control for inode */
static inline struct inode_delta_dirty *tux3_inode_ddc(struct inode *inode,
						       unsigned delta)
{
	return &tux_inode(inode)->i_ddc[tux3_delta(delta)];
}

static inline struct tux3_inode *i_ddc_to_inode(struct inode_delta_dirty *i_ddc,
						unsigned delta)
{
	return container_of(i_ddc, struct tux3_inode, i_ddc[tux3_delta(delta)]);
}

/* Get per-delta dirty buffers list from inode */
static inline struct list_head *tux3_dirty_buffers(struct inode *inode,
						   unsigned delta)
{
	return &tux3_inode_ddc(inode, delta)->dirty_buffers;
}

/* inode flags */
enum {
	/* Deferred inum allocation, and not stored into itree yet. */
	TUX3_I_DEFER_INUM	= 0,

	/* No per-delta buffers, and no page forking */
	TUX3_I_NO_DELTA		= 29,
	/* Accessed from backend only, and flushed on unify */
	TUX3_I_UNIFY		= 30,
	/*
	 * If no-flush flag is set, tux3_flush_inodes() doesn't flush. Some
	 * inodes have to be flushed by custom timing, and it is flushed by
	 * tux3_flush_inode_internal() instead.
	 *
	 * This inode must be pinned until umount, to flush by
	 * tux3_flush_inode_internal().
	 */
	TUX3_I_NO_FLUSH		= 31,
};

static inline void tux3_inode_set_flag(int bit, struct inode *inode)
{
	set_bit(bit, &tux_inode(inode)->flags);
}

static inline void tux3_inode_clear_flag(int bit, struct inode *inode)
{
	clear_bit(bit, &tux_inode(inode)->flags);
}

static inline int tux3_inode_test_flag(int bit, struct inode *inode)
{
	return test_bit(bit, &tux_inode(inode)->flags);
}

struct tux_iattr {
	kuid_t	uid;
	kgid_t	gid;
	umode_t	mode;
	dev_t	rdev;
};

static inline struct btree *itree_btree(struct sb *sb)
{
	return &sb->itree;
}

static inline struct btree *otree_btree(struct sb *sb)
{
	return &sb->otree;
}

#define TUX_MAX_LINKS (1 << 15) /* arbitrary limit, increase it */

#define TUX_NAME_LEN 255

/* Directory entry */
struct tux3_dirent {
	__be64 inum;
	__be16 rec_len;
	u8 name_len, type;
	char name[];
	/*
	 * On 64bit arch sizeof(struct tux3_dirent) == 16. We should use
	 * offsetof(struct tux3_dirent, name) instead.
	 */
	/* u32 __pad; */
};

struct btree_key_range {
	tuxkey_t start;
	unsigned len;
};

enum btree_result {
	BTREE_DO_RETRY = 0,
	BTREE_DO_DIRTY,
	BTREE_DO_SPLIT,
};

struct btree_ops {
	void (*btree_init)(struct btree *btree);
	int (*leaf_init)(struct btree *btree, void *leaf);
	tuxkey_t (*leaf_split)(struct btree *btree, tuxkey_t hint, void *from, void *into);
	/* return value: 1 - modified, 0 - not modified, < 0 - error */
	int (*leaf_chop)(struct btree *btree, tuxkey_t start, u64 len, void *leaf);
	/* return value: 1 - merged, 0 - couldn't merge.
	 * "into" must be left leaf and "from" is right leaf. (left < right) */
	int (*leaf_merge)(struct btree *btree, void *into, void *from);
	/* return value: < 0 - error, 0 >= - btree_result */
	int (*leaf_pre_write)(struct btree *btree, tuxkey_t key_bottom, tuxkey_t key_limit, void *leaf, struct btree_key_range *key);
	/* return value: < 0 - error, 0 >= - btree_result */
	int (*leaf_write)(struct btree *btree, tuxkey_t key_bottom, tuxkey_t key_limit, void *leaf, struct btree_key_range *key, tuxkey_t *split_hint);
	int (*leaf_read)(struct btree *btree, tuxkey_t key_bottom, tuxkey_t key_limit, void *leaf, struct btree_key_range *key);

	void *private_ops;

	/*
	 * for debugging
	 */
	int (*leaf_sniff)(struct btree *btree, void *leaf);
	/* return value: 1 - can free, 0 - can't free */
	int (*leaf_can_free)(struct btree *btree, void *leaf);
};

/* Information for replay */
struct replay {
	struct sb *sb;

	/* For orphan.c */
	struct list_head log_orphan_add;   /* To remember LOG_ORPHAN_ADD */
	struct list_head orphan_in_otree; /* Orphan inodes in sb->otree */

	/* For replay.c */
	void *unify_pos;	/* position of unify log in a log block */
	block_t unify_index;	/* index of a log block including unify log */
	block_t blocknrs[];	/* block address of log blocks */
};

/* Redirect ptr which is pointing data of src from src to dst */
static inline void *ptr_redirect(void *ptr, void *src, void *dst)
{
	if (ptr) {
		assert(ptr >= src);
		return dst + (ptr - src);
	}
	return NULL;
}

#ifdef __KERNEL__
static inline struct inode *buffer_inode(struct buffer_head *buffer)
{
	return buffer->b_page->mapping->host;
}

/* Get logical index of buffer */
static inline block_t bufindex(struct buffer_head *buffer)
{
	struct page *page = buffer->b_page;
	/* FIXME: maybe we want to remove buffer->b_size */
#if BITS_PER_LONG == 64
	return (page_offset(page) + bh_offset(buffer)) / buffer->b_size;
#else
	const int blockbits = ffs(buffer->b_size) - 1;
	return (page_offset(page) + bh_offset(buffer)) >> blockbits;
#endif
}

/* commit.c */
long tux3_writeback(struct super_block *super, struct bdi_writeback *wb,
		    struct wb_writeback_work *work);

/* dir.c */
extern const struct file_operations tux_dir_fops;
extern const struct inode_operations tux_dir_iops;

/* filemap.c */
int tux3_get_block(struct inode *inode, sector_t iblock,
		   struct buffer_head *bh_result, int create);
struct buffer_head *__get_buffer(struct page *page, int offset);
void tux3_try_cancel_dirty_page(struct page *page);
int tux3_file_mmap(struct file *file, struct vm_area_struct *vma);
extern const struct address_space_operations tux_file_aops;
extern const struct address_space_operations tux_symlink_aops;
extern const struct address_space_operations tux_blk_aops;
extern const struct address_space_operations tux_vol_aops;

/* inode.c */
int tux3_getattr(struct user_namespace *mnt_uerns, const struct path *path,
		 struct kstat *stat, u32 request_mask, unsigned int flags);
int tux3_sync_file(struct file *file, loff_t start, loff_t end, int datasync);
int tux3_no_update_time(struct inode *inode, struct timespec64 *time, int flags);

/* symlink.c */
extern const struct inode_operations tux_symlink_iops;

/* utility.c */
int devio_sync(enum req_opf req_opf, unsigned int req_flags,
	       struct block_device *dev, loff_t offset, void *data,
	       unsigned len);
int blockio(enum req_opf req_opf, unsigned int req_flags,
	    struct sb *sb, struct buffer_head *buffer, block_t block,
	    bio_end_io_t endio, void *info);
int blockio_sync(enum req_opf req_opf, unsigned int req_flags, struct sb *sb,
		 struct buffer_head *buffer, block_t block);
int blockio_vec(struct bufvec *bufvec, block_t block, unsigned count);

#define tux3_msg(sb, fmt, ...)						\
	__tux3_msg(sb, KERN_INFO, "", fmt, ##__VA_ARGS__)
#define __tux3_err(sb, func, line, fmt, ...)				\
	__tux3_msg(sb, KERN_ERR, " error",				\
		   "%s:%d: " fmt, func, line, ##__VA_ARGS__)
#define tux3_err(sb, fmt, ...)						\
	__tux3_err(sb, __func__, __LINE__, fmt, ##__VA_ARGS__)
#define tux3_warn(sb, fmt, ...)					\
	__tux3_msg(sb, KERN_WARNING, " warning", fmt, ##__VA_ARGS__)

/* temporary hack for buffer */
struct buffer_head *peekblk(struct address_space *mapping, block_t iblock);
struct buffer_head *blockread(struct address_space *mapping, block_t iblock);
struct buffer_head *blockget(struct address_space *mapping, block_t iblock);
#endif /* __KERNEL__ */

/* balloc.c */
void countmap_put(struct countmap_pin *countmap_pin);
int countmap_used(struct sb *sb, block_t group);
void bitmap_dump(struct inode *inode, block_t start, block_t count);
int balloc_find_range(struct sb *sb,
	struct block_segment *seg, int maxsegs, int *segs,
	block_t start, block_t range, unsigned *blocks);
int balloc_find(struct sb *sb,
	struct block_segment *seg, int maxsegs, int *segs,
	unsigned *blocks);
int balloc_use(struct sb *sb, struct block_segment *seg, int segs);
int balloc_segs(struct sb *sb,
	struct block_segment *seg, int maxsegs, int *segs,
	unsigned *blocks);
block_t balloc_one(struct sb *sb);
int bfree_segs(struct sb *sb, struct block_segment *seg, int segs);
int bfree(struct sb *sb, block_t start, unsigned blocks);
int replay_update_bitmap(struct replay *rp, block_t start, unsigned blocks, int set);

/* btree.c */
extern struct root no_root;

/* Does this root has direct extent? */
static inline int has_direct_extent(struct btree *btree)
{
	return btree->root.direct;
}

/* Does this root has bnode/leaf? */
static inline int has_root(struct btree *btree)
{
	return !has_direct_extent(btree) && btree->root.depth > 0;
}

/* Doesn't have both btree or direct extent? */
static inline int has_no_root(struct btree *btree)
{
	return btree->root.depth == 0;
}

void btree_init_param(struct sb *sb);
struct buffer_head *cursor_leafbuf(struct cursor *cursor);
void release_cursor(struct cursor *cursor);
struct cursor *alloc_cursor(struct btree *btree, int);
void free_cursor(struct cursor *cursor);

void init_btree(struct btree *btree, struct sb *sb, struct root root, struct btree_ops *ops);
int btree_alloc_empty(struct btree *btree);
int btree_free_empty(struct btree *btree);
struct buffer_head *new_leaf(struct btree *btree);
tuxkey_t cursor_next_key(struct cursor *cursor);
tuxkey_t cursor_this_key(struct cursor *cursor);
int btree_probe(struct cursor *cursor, tuxkey_t key);
typedef int (*btree_traverse_func_t)(struct btree *btree, tuxkey_t key_bottom,
				     tuxkey_t key_limit, void *leaf,
				     tuxkey_t key, u64 len, void *data);
int btree_traverse(struct cursor *cursor, tuxkey_t key, u64 len,
		   btree_traverse_func_t func, void *data);
int btree_chop(struct btree *btree, tuxkey_t start, u64 len);
int btree_insert_leaf(struct cursor *cursor, tuxkey_t key, struct buffer_head *leafbuf);
void *btree_expand(struct cursor *cursor, tuxkey_t key, unsigned newsize);
int noop_pre_write(struct btree *btree, tuxkey_t key_bottom, tuxkey_t key_limit,
		   void *leaf, struct btree_key_range *key);
int btree_write(struct cursor *cursor, struct btree_key_range *key);
int btree_read(struct cursor *cursor, struct btree_key_range *key);
int cursor_redirect(struct cursor *cursor);
int replay_bnode_redirect(struct replay *rp, block_t oldblock, block_t newblock);
int replay_bnode_root(struct replay *rp, block_t root, unsigned count,
		      block_t left, block_t right, tuxkey_t rkey);
int replay_bnode_split(struct replay *rp, block_t src, unsigned pos, block_t dst);
int replay_bnode_add(struct replay *rp, block_t parent, block_t child, tuxkey_t key);
int replay_bnode_update(struct replay *rp, block_t parent, block_t child, tuxkey_t key);
int replay_bnode_merge(struct replay *rp, block_t src, block_t dst);
int replay_bnode_del(struct replay *rp, block_t bnode, tuxkey_t key, unsigned count);
int replay_bnode_adjust(struct replay *rp, block_t bnode, tuxkey_t from, tuxkey_t to);

/* commit.c */
void defer_bfree_init(struct defree *defree);
void destroy_defer_bfree(struct defree *defree);
int defer_bfree(struct sb *sb, struct defree *defree, block_t block,
		unsigned count);

block_t nospc_min_reserve(void);
int nospc_wait_and_check(struct sb *sb, int cost, int limit);
void nospc_init_balance(struct sb *sb);
unsigned nospc_one_page_cost(struct inode *inode);

static inline int change_active(void)
{
	return !!current->journal_info;
}

void tux3_start_backend(struct sb *sb);
void tux3_end_backend(void);
int tux3_under_backend(struct sb *sb);
void tux3_delta_init(struct sb *sb);
void tux3_delta_setup(struct sb *sb);
unsigned tux3_get_current_delta(void);
unsigned tux3_inode_delta(struct inode *inode);
int sync_current_delta(struct sb *sb);
void change_begin_atomic(struct sb *sb);
void change_end_atomic(struct sb *sb);
void change_begin_atomic_nested(struct sb *sb, void **ptr);
void change_end_atomic_nested(struct sb *sb, void *ptr);
void change_begin_nocheck(struct sb *sb);
int change_begin_nospc(struct sb *sb, int cost, int limit);
int change_begin(struct sb *sb, int cost);
int change_begin_unlink(struct sb *sb, int cost, bool orphaned);
int change_end(struct sb *sb);

/* dir.c */
void tux_set_entry(struct buffer_head *buffer, struct tux3_dirent *entry,
		   inum_t inum, umode_t mode);
void tux_update_dirent(struct inode *dir, struct buffer_head *buffer,
		       struct tux3_dirent *entry, struct inode *new_inode);
loff_t tux_alloc_entry(struct inode *dir, const char *name, unsigned len,
		       loff_t *size, struct buffer_head **hold);
struct inode *__tux_create_dirent(struct inode *dir, const struct qstr *qstr,
				  struct inode *inode, struct tux_iattr *iattr);
static inline struct inode *
tux_create_dirent_and_inode(struct inode *dir, const struct qstr *qstr,
			    struct tux_iattr *iattr)
{
	return __tux_create_dirent(dir, qstr, NULL, iattr);
}
static inline int tux_create_dirent(struct inode *dir, const struct qstr *qstr,
				    struct inode *inode)
{
	struct inode *ret = __tux_create_dirent(dir, qstr, inode, NULL);
	return IS_ERR(ret) ? PTR_ERR(ret) : 0;
}
struct tux3_dirent *tux_find_entry(struct inode *dir, const char *name,
				   unsigned len, struct buffer_head **result,
				   loff_t size);
struct tux3_dirent *tux_find_dirent(struct inode *dir, const struct qstr *qstr,
				    struct buffer_head **result);
int tux_delete_entry(struct inode *dir, struct buffer_head *buffer,
		     struct tux3_dirent *entry);
int tux_delete_dirent(struct inode *dir, struct buffer_head *buffer,
		      struct tux3_dirent *entry);
int tux_readdir(struct file *file, struct dir_context *ctx);
int tux_dir_is_empty(struct inode *dir);

/* dleaf.c */
extern struct btree_ops dtree_ops;

/* filemap.c */
void remember_dleaf(struct sb *sb, struct buffer_head *leafbuf);
int dtree_chop(struct btree *btree, tuxkey_t start, u64 len);
int tux3_filemap_overwrite_io(struct bufvec *bufvec);
int tux3_filemap_redirect_io(struct bufvec *bufvec);
int tux3_truncate_partial_block(struct inode *inode, loff_t newsize);

/* iattr.c */
void dump_attrs(struct inode *inode);
void *encode_kind(void *attrs, unsigned kind, unsigned version);
void *decode_kind(void *attrs, unsigned *kind, unsigned *version);
extern struct ileaf_attr_ops iattr_ops;

/* ileaf.c */
struct ileaf;
int ileaf_inum_exists(struct btree *btree, struct ileaf *ileaf, inum_t inum);
int ileaf_find_free(struct btree *btree, tuxkey_t key_bottom,
		    tuxkey_t key_limit, void *leaf,
		    tuxkey_t key, u64 len, void *data);
struct ileaf_enumrate_cb {
	int (*callback)(struct btree *btree, inum_t inum, void *attrs,
			unsigned size, void *data);
	void *data;
};
int ileaf_enumerate(struct btree *btree, tuxkey_t key_bottom,
		    tuxkey_t key_limit, void *leaf,
		    tuxkey_t key, u64 len, void *data);
extern struct btree_ops itree_ops;
extern struct btree_ops otree_ops;

/* inode.c */
void tux3_inode_copy_attrs(struct inode *inode, unsigned delta);
struct inode *tux_new_volmap(struct sb *sb);
struct inode *tux_new_logmap(struct sb *sb);
struct inode *tux_new_inode(struct sb *sb, struct inode *dir,
			    struct tux_iattr *iattr);
struct tux3_idefer_map *tux3_alloc_idefer_map(void);
void tux3_free_idefer_map(struct tux3_idefer_map *map);
int __init tux3_init_idefer_cache(void);
void tux3_destroy_idefer_cache(void);
void cancel_defer_alloc_inum(struct inode *inode);
struct inode *tux_create_inode(struct inode *dir, loff_t dir_pos,
			       struct tux_iattr *iattr);
struct inode *tux_create_specific_inode(struct sb *sb, struct inode *dir,
					inum_t inum, struct tux_iattr *iattr);
struct inode *__tux3_iget(struct sb *sb, inum_t inum);
static inline struct inode *tux3_iget(struct sb *sb, inum_t inum)
{
	if (inum != TUX_ROOTDIR_INO && inum < TUX_NORMAL_INO)
		return ERR_PTR(-EFSCORRUPTED);
	return __tux3_iget(sb, inum);
}
struct inode *tux3_iget(struct sb *sb, inum_t inum);
struct inode *tux3_ilookup_nowait(struct sb *sb, inum_t inum);
struct inode *tux3_ilookup(struct sb *sb, inum_t inum);
int tux3_save_inode(struct inode *inode, struct tux3_iattr_data *idata,
		    unsigned delta);
int tux3_purge_inode(struct inode *inode, struct tux3_iattr_data *idata,
		     unsigned delta);
int tux3_drop_inode(struct inode *inode);
void tux3_evict_inode(struct inode *inode);
int tux3_setattr(struct user_namespace *mnt_userns,
		 struct dentry *dentry, struct iattr *iattr);
void iget_if_dirty(struct inode *inode);

/* log.c */
extern unsigned log_size[];
void log_next(struct sb *sb);
void log_drop(struct sb *sb);
void log_finish(struct sb *sb);
void log_finish_cycle(struct sb *sb, int discard);
int tux3_logmap_io(struct bufvec *bufvec);
void log_balloc(struct sb *sb, block_t block, unsigned count);
void log_bfree(struct sb *sb, block_t block, unsigned count);
void log_bfree_on_unify(struct sb *sb, block_t block, unsigned count);
void log_bfree_relog(struct sb *sb, block_t block, unsigned count);
void log_leaf_redirect(struct sb *sb, block_t oldblock, block_t newblock);
void log_leaf_free(struct sb *sb, block_t leaf);
void log_bnode_redirect(struct sb *sb, block_t oldblock, block_t newblock);
void log_bnode_root(struct sb *sb, block_t root, unsigned count,
		    block_t left, block_t right, tuxkey_t rkey);
void log_bnode_split(struct sb *sb, block_t src, unsigned pos, block_t dst);
void log_bnode_add(struct sb *sb, block_t parent, block_t child, tuxkey_t key);
void log_bnode_update(struct sb *sb, block_t parent, block_t child,
		      tuxkey_t key);
void log_bnode_merge(struct sb *sb, block_t src, block_t dst);
void log_bnode_del(struct sb *sb, block_t node, tuxkey_t key, unsigned count);
void log_bnode_adjust(struct sb *sb, block_t node, tuxkey_t from, tuxkey_t to);
void log_bnode_free(struct sb *sb, block_t bnode);
void log_orphan_add(struct sb *sb, unsigned version, tuxkey_t inum);
void log_orphan_del(struct sb *sb, unsigned version, tuxkey_t inum);
void log_freeblocks(struct sb *sb, block_t freeblocks);
void log_delta(struct sb *sb);
void log_unify(struct sb *sb);

typedef int (*unstash_t)(u64 val, void *data);
void stash_init(struct stash *stash);
int stash_value(struct stash *stash, u64 value);
int unstash(struct stash *stash, unstash_t actor, void *data);
int stash_walk(struct stash *stash, unstash_t actor, void *data);
void empty_stash(struct stash *stash);

/* orphan.c */
long long clean_orphan_list(struct list_head *head);
extern struct ileaf_attr_ops oattr_ops;
void tux3_orphan_init(struct sb *sb);
int tux3_unify_orphan_add(struct sb *sb, struct list_head *orphan_add);
int tux3_unify_orphan_del(struct sb *sb, struct list_head *orphan_del);
int tux3_make_orphan_add(struct inode *inode);
int tux3_make_orphan_del(struct inode *inode);
int replay_orphan_add(struct replay *rp, unsigned version, inum_t inum);
int replay_orphan_del(struct replay *rp, unsigned version, inum_t inum);
void replay_iput_orphan_inodes(struct sb *sb,
			       struct list_head *orphan_in_otree,
			       int destroy);
int replay_load_orphan_inodes(struct replay *rp);

/* super.c */
struct replay *tux3_init_fs(struct sb *sbi);
extern const struct tux3_mount_opt tux3_default_mopt;

/* policy.c */
inum_t policy_inum(struct inode *dir, loff_t where, struct tux_iattr *iattr);
void policy_inode_init(inum_t *previous);
void policy_inode(struct inode *inode, inum_t *previous);
void policy_extents(struct bufvec *bufvec);

/* replay.c */
struct replay *replay_stage1(struct sb *sb);
int replay_stage2(struct replay *rp);
int replay_stage3(struct replay *rp, int apply);

/* utility.c */
void __printf(4, 5)
__tux3_msg(struct sb *sb, const char *level, const char *prefix,
	   const char *fmt, ...);
void __printf(1, 2)
__tux3_dbg(const char *fmt, ...);
#define tux3_dbg(fmt , ...)						\
	__tux3_dbg("%s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
void __printf(4, 5)
__tux3_fs_error(struct sb *sb, const char *func, unsigned int line,
		const char *fmt, ...);
#define tux3_fs_error(sb, fmt, ...)	 \
	__tux3_fs_error(sb, __func__, __LINE__, fmt , ##__VA_ARGS__)

void hexdump(void *data, unsigned size);
void set_bits(u8 *bitmap, unsigned start, unsigned count);
void clear_bits(u8 *bitmap, unsigned start, unsigned count);
int all_set(u8 *bitmap, unsigned start, unsigned count);
int all_clear(u8 *bitmap, unsigned start, unsigned count);
int bytebits(u8 c);

/* writeback.c */
void tux3_set_inode_always_dirty(struct inode *inode);
void tux3_mark_btree_dirty(struct btree *btree);
void __tux3_mark_inode_dirty(struct inode *inode, int flags);
#ifndef __KERNEL__
void __tux3_clear_dirty_inode(struct inode *inode, unsigned delta);
#endif
static inline void tux3_mark_inode_dirty(struct inode *inode)
{
	__tux3_mark_inode_dirty(inode, I_DIRTY);
}
static inline void tux3_mark_inode_dirty_sync(struct inode *inode)
{
	__tux3_mark_inode_dirty(inode, I_DIRTY_SYNC);
}

void tux3_dirty_inode(struct inode *inode, int flags);
void tux3_mark_inode_to_delete(struct inode *inode);
void tux3_iattrdirty(struct inode *inode);
void tux3_xattrdirty(struct inode *inode);
void tux3_xattr_read_and_clear(struct inode *inode);
void tux3_clear_dirty_inode(struct inode *inode);
void __tux3_mark_buffer_dirty(struct buffer_head *buffer, unsigned delta);
void tux3_mark_buffer_dirty(struct buffer_head *buffer);
void tux3_mark_buffer_unify(struct buffer_head *buffer);
void tux3_mark_inode_orphan(struct tux3_inode *tuxnode);
int tux3_inode_is_orphan(struct tux3_inode *tuxnode);
int tux3_flush_inode_internal(struct inode *inode, unsigned delta,
			      unsigned int req_flags);
int tux3_flush_inode(struct inode *inode, unsigned delta,
		     unsigned int req_flags);
int tux3_flush_inodes(struct sb *sb, unsigned delta);
int tux3_has_dirty_inodes(struct sb *sb, unsigned delta);
void tux3_clear_dirty_inodes(struct sb *sb, unsigned delta);
unsigned tux3_check_tuxinode_state(struct inode *inode);

/* xattr.c */
#ifndef ENOATTR
#define ENOATTR ENODATA
#endif

void atable_init_base(struct sb *sb);
int xcache_dump(struct inode *inode);
void free_xcache(struct inode *inode);
int new_xcache(struct inode *inode, unsigned size);
int xcache_remove_all(struct inode *inode);
int get_xattr(struct inode *inode, const char *name, unsigned len,
	      void *data, unsigned size);
int set_xattr(struct inode *inode, const char *name, unsigned len,
	      const void *data, unsigned size, unsigned flags);
int del_xattr(struct inode *inode, const char *name, unsigned len);
int list_xattr(struct inode *inode, char *text, size_t size);
unsigned encode_xsize(struct inode *inode);
void *encode_xattrs(struct inode *inode, void *attrs, unsigned size);
unsigned decode_xsize(struct inode *inode, void *attrs, unsigned size);
void *decode_xattr(struct inode *inode, void *attrs);

static inline struct buffer_head *vol_find_get_block(struct sb *sb, block_t block)
{
	return peekblk(mapping(sb->volmap), block);
}

static inline struct buffer_head *vol_getblk(struct sb *sb, block_t block)
{
	return blockget(mapping(sb->volmap), block);
}

static inline struct buffer_head *vol_bread(struct sb *sb, block_t block)
{
	return blockread(mapping(sb->volmap), block);
}

#include "dirty-buffer.h"	/* remove this after atomic commit */
#endif /* !TUX3_H */
