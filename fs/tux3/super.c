/*
 * Superblock handling.
 *
 * Copyright (c) 2008-2014 Daniel Phillips
 * Copyright (c) 2008-2014 OGAWA Hirofumi
 */

#include "tux3.h"
#include "filemap_hole.h"
#ifdef __KERNEL__
#include <linux/module.h>
#include <linux/statfs.h>
#include <linux/parser.h>
#include <linux/seq_file.h>
#define trace trace_on

/* FIXME: this should be mount option? */
int tux3_trace;
module_param(tux3_trace, int, 0644);
#endif

/* This will go to include/linux/magic.h */
#ifndef TUX3_SUPER_MAGIC
#define TUX3_SUPER_MAGIC	0x74757833
#endif

static struct kmem_cache *tux_inode_cachep;

static void tux3_inode_init_once(void *mem)
{
	struct tux3_inode *tuxnode = mem;
	struct inode *inode = &tuxnode->vfs_inode;
	int i;

	INIT_LIST_HEAD(&tuxnode->orphan_list);
	spin_lock_init(&tuxnode->hole_extents_lock);
	INIT_LIST_HEAD(&tuxnode->hole_extents);
	init_rwsem(&tuxnode->truncate_lock);
	spin_lock_init(&tuxnode->lock);
	/* Initialize inode_delta_dirty */
	for (i = 0; i < ARRAY_SIZE(tuxnode->i_ddc); i++) {
		INIT_LIST_HEAD(&tuxnode->i_ddc[i].dirty_buffers);
		INIT_LIST_HEAD(&tuxnode->i_ddc[i].dirty_holes);
		INIT_LIST_HEAD(&tuxnode->i_ddc[i].dirty_list);
		/* For debugging, set invalid value */
		tuxnode->i_ddc[i].idata.i_mode = TUX3_INVALID_IDATA;
	}

	/* Initialize generic part */
	inode_init_once(inode);
}

static void tux3_inode_init_always(struct tux3_inode *tuxnode)
{
	static struct timespec64 epoch;
	struct inode *inode = &tuxnode->vfs_inode;

	tuxnode->btree		= (struct btree){ };
	tuxnode->flags		= 0;
	tuxnode->xcache		= NULL;
	tuxnode->generic	= 0;
	tuxnode->state		= 0;
#ifdef __KERNEL__
	tuxnode->io		= NULL;
#endif

	/* uninitialized stuff by alloc_inode() */
	inode_set_iversion(inode, 1);
	inode->i_atime		= epoch;
	inode->i_mtime		= epoch;
	inode->i_ctime		= epoch;
	inode->i_mode		= 0;
}

static struct inode *tux3_alloc_inode(struct super_block *sb)
{
	struct tux3_inode *tuxnode;

	tuxnode = kmem_cache_alloc(tux_inode_cachep, GFP_KERNEL);
	if (!tuxnode)
		return NULL;

	tux3_inode_init_always(tuxnode);

	return &tuxnode->vfs_inode;
}

static int i_ddc_is_clean(struct inode *inode)
{
	struct tux3_inode *tuxnode = tux_inode(inode);
	int i;

	for (i = 0; i < ARRAY_SIZE(tuxnode->i_ddc); i++) {
		if (!list_empty(&tuxnode->i_ddc[i].dirty_buffers) ||
		    !list_empty(&tuxnode->i_ddc[i].dirty_holes) ||
		    !list_empty(&tuxnode->i_ddc[i].dirty_list))
			return 0;
	}

	return 1;
}

static void tux3_free_inode(struct inode *inode)
{
	/* Those must be clean, tux3_inode_init_always() doesn't init. */
	assert(list_empty(&tux_inode(inode)->orphan_list));
	assert(list_empty(&tux_inode(inode)->hole_extents));
	assert(i_ddc_is_clean(inode));

	kmem_cache_free(tux_inode_cachep, tux_inode(inode));
}

static int __init tux3_init_inodecache(void)
{
	tux_inode_cachep = kmem_cache_create("tux3_inode_cache",
			sizeof(struct tux3_inode), 0,
			(SLAB_RECLAIM_ACCOUNT|SLAB_MEM_SPREAD|SLAB_ACCOUNT),
			tux3_inode_init_once);
	if (tux_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

static void tux3_destroy_inodecache(void)
{
	/*
	 * Make sure all delayed rcu free inodes are flushed before we
	 * destroy cache.
	 */
	rcu_barrier();
	kmem_cache_destroy(tux_inode_cachep);
}

#ifdef __KERNEL__
#define BUFFER_LINK	b_assoc_buffers
#else
#define BUFFER_LINK	link
#endif

static void cleanup_dirty_buffers(struct inode *inode, struct list_head *head,
				  unsigned delta)
{
	struct buffer_head *buffer, *n;

	list_for_each_entry_safe(buffer, n, head, BUFFER_LINK) {
		trace(">>> clean inum %Lx, buffer %Lx, count %d",
		      tux_inode(inode)->inum, bufindex(buffer),
		      bufcount(buffer));
		assert(buffer_dirty(buffer));
		tux3_clear_buffer_dirty(buffer, delta);
	}
}

static void cleanup_dirty_inode(struct inode *inode)
{
	if (inode->i_state & I_DIRTY) {
		trace(">>> clean inum %Lx, i_count %d, i_state %lx",
		      tux_inode(inode)->inum, atomic_read(&inode->i_count),
		      inode->i_state);
		cancel_defer_alloc_inum(inode);
		tux3_clear_dirty_inode(inode);
	}
}

/*
 * Some inode/buffers are always (re-)dirtied, so we have to cleanup
 * those for umount.
 */
static void cleanup_dirty_for_umount(struct sb *sb)
{
	unsigned unify = sb->unify;
	struct list_head *head;

	/*
	 * Pinned buffer and bitmap are not flushing always, it is
	 * normal. So, this clean those for unmount.
	 */
	if (sb->bitmap) {
		head = tux3_dirty_buffers(sb->bitmap, unify);
		cleanup_dirty_buffers(sb->bitmap, head, unify);
		cleanup_dirty_inode(sb->bitmap);
	}
	if (sb->countmap) {
		head = tux3_dirty_buffers(sb->countmap, unify);
		cleanup_dirty_buffers(sb->countmap, head, unify);
		cleanup_dirty_inode(sb->countmap);
	}
	if (sb->volmap) {
		cleanup_dirty_buffers(sb->volmap, &sb->unify_buffers, unify);
		/*
		 * FIXME: mark_buffer_dirty() for unify buffers marks
		 * volmap as I_DIRTY_PAGES (we don't need I_DIRTY_PAGES
		 * actually) without changing tuxnode->state.
		 *
		 * So this is called to clear I_DIRTY_PAGES.
		 */
		cleanup_dirty_inode(sb->volmap);
	}

	/* orphan_add should be empty */
	assert(sb->orphan.count == 0);
	assert(list_empty(&sb->orphan.add_head));
	/* Deferred orphan deletion request is not flushed for each delta  */
	sb->orphan.count_del -= clean_orphan_list(&sb->orphan.del_head);
	assert(sb->orphan.count_del == 0);

	/* defree must be flushed for each delta */
	assert(sb->defree.blocks == 0);
}

static void __tux3_put_super(struct sb *sbi)
{
	cleanup_dirty_for_umount(sbi);

	/* All forked buffers should be freed here */
	free_forked_buffers(sbi, NULL, 1);

	destroy_defer_bfree(&sbi->deunify);
	destroy_defer_bfree(&sbi->defree);

	countmap_put(&sbi->countmap_pin);

	iput(sbi->rootdir);
	sbi->rootdir = NULL;
	iput(sbi->atable);
	sbi->atable = NULL;
	iput(sbi->vtable);
	sbi->vtable = NULL;
	iput(sbi->bitmap);
	sbi->bitmap = NULL;
	iput(sbi->countmap);
	sbi->countmap = NULL;
	iput(sbi->logmap);
	sbi->logmap = NULL;
	iput(sbi->volmap);
	sbi->volmap = NULL;

	tux3_free_idefer_map(sbi->idefer_map);
	sbi->idefer_map = NULL;
	/* FIXME: add more sanity check */
	assert(link_empty(&sbi->forked_buffers));
}

static struct inode *create_internal_inode(struct sb *sbi, inum_t inum,
					   struct tux_iattr *iattr)
{
	static struct tux_iattr null_iattr;
	struct inode *inode;

	if (iattr == NULL)
		iattr = &null_iattr;

	inode = tux_create_specific_inode(sbi, NULL, inum, iattr);
	if (!IS_ERR(inode)) {
		assert(tux_inode(inode)->inum == inum);
		unlock_new_inode(inode);
	}
	return inode;
}

/*
 * Internal inode (e.g. bitmap inode) yet may not be written. So, if
 * there is no inode, create inode instead.
 */
static struct inode *iget_or_create_inode(struct sb *sbi, inum_t inum)
{
	struct inode *inode;

	inode = __tux3_iget(sbi, inum);
	if (IS_ERR(inode) && PTR_ERR(inode) == -ENOENT)
		inode = create_internal_inode(sbi, inum, NULL);
	return inode;
}

struct replay *tux3_init_fs(struct sb *sbi)
{
	struct replay *rp = NULL;
	struct inode *inode;
	char *name;
	int err;

	err = -ENOMEM;

	/* Prepare non on-disk inodes */
	sbi->volmap = tux_new_volmap(sbi);
	if (!sbi->volmap)
		goto error;

	sbi->logmap = tux_new_logmap(sbi);
	if (!sbi->logmap)
		goto error;

	/* Replay physical structures */
	rp = replay_stage1(sbi);
	if (IS_ERR(rp)) {
		err = PTR_ERR(rp);
		goto error;
	}

	/* Load internal inodes */
	inode = iget_or_create_inode(sbi, TUX_BITMAP_INO);
	if (IS_ERR(inode)) {
		name = "bitmap";
		goto error_inode;
	}
	sbi->bitmap = inode;

	inode = iget_or_create_inode(sbi, TUX_COUNTMAP_INO);
	if (IS_ERR(inode)) {
		name = "countmap";
		goto error_inode;
	}
	sbi->countmap = inode;
#if 0
	inode = __tux3_iget(sbi, TUX_VTABLE_INO);
	if (IS_ERR(inode)) {
		name = "vtable";
		goto error_inode;
	}
	sbi->vtable = inode;
#endif
	inode = __tux3_iget(sbi, TUX_ATABLE_INO);
	if (IS_ERR(inode)) {
		name = "atable";
		goto error_inode;
	}
	sbi->atable = inode;

	inode = __tux3_iget(sbi, TUX_ROOTDIR_INO);
	if (IS_ERR(inode)) {
		name = "rootdir";
		goto error_inode;
	}
	sbi->rootdir = inode;

	err = replay_stage2(rp);
	if (err) {
		rp = NULL;
		goto error;
	}

	/* Initialize ENOSPC state with latest freeblocks. */
	nospc_init_balance(sbi);

	return rp;

error_inode:
	err = PTR_ERR(inode);
	tux3_err(sbi, "failed to load %s inode (err %d)", name, err);
error:
	if (!IS_ERR_OR_NULL(rp))
		replay_stage3(rp, 0);
	__tux3_put_super(sbi);

	return ERR_PTR(err);
}

/* Initialize the lock and list */
static int init_sb(struct sb *sb)
{
	int i;

	/* Initialize sb */

	tux3_delta_init(sb);

	tux3_orphan_init(sb);
	defer_bfree_init(&sb->defree);
	defer_bfree_init(&sb->deunify);
	INIT_LIST_HEAD(&sb->unify_buffers);
	INIT_LIST_HEAD(&sb->phase2_buffers);

	spin_lock_init(&sb->countmap_lock);
	spin_lock_init(&sb->forked_buffers_lock);
	init_link_circular(&sb->forked_buffers);
	spin_lock_init(&sb->dirty_inodes_lock);

	/* Initialize sb_delta_dirty */
	for (i = 0; i < ARRAY_SIZE(sb->s_ddc); i++)
		INIT_LIST_HEAD(&sb->s_ddc[i].dirty_inodes);

	sb->idefer_map = tux3_alloc_idefer_map();
	if (!sb->idefer_map)
		return -ENOMEM;

	return 0;
}

static void setup_roots(struct sb *sb, struct disksuper *super)
{
	u64 iroot_val = be64_to_cpu(super->iroot);
	u64 oroot_val = be64_to_cpu(super->oroot);
	init_btree(itree_btree(sb), sb, unpack_root(iroot_val), &itree_ops);
	init_btree(otree_btree(sb), sb, unpack_root(oroot_val), &otree_ops);
}

static loff_t calc_maxbytes(loff_t blocksize)
{
	return min_t(loff_t, blocksize << MAX_BLOCKS_BITS, MAX_LFS_FILESIZE);
}

/* FIXME: Should goes into core */
static inline u64 roundup_pow_of_two64(u64 n)
{
	return 1ULL << fls64(n - 1);
}

/* Setup sb by on-disk super block */
static void __setup_sb(struct sb *sb, struct disksuper *super)
{
	tux3_delta_setup(sb);

	sb->blockbits = be16_to_cpu(super->blockbits);
	sb->volblocks = be64_to_cpu(super->volblocks);
	sb->version = 0;	/* FIXME: not yet implemented */

	sb->blocksize = 1 << sb->blockbits;
	sb->blockmask = (1 << sb->blockbits) - 1;
#ifdef __KERNEL__
	sb->blocks_per_page_bits = PAGE_SHIFT - sb->blockbits;
#else
	sb->blocks_per_page_bits = 0;
#endif
	sb->blocks_per_page = 1 << sb->blocks_per_page_bits;
	sb->groupbits = 13; // FIXME: put in disk super?
	sb->volmask = roundup_pow_of_two64(sb->volblocks) - 1;

	/* Initialize parameter for btree */
	btree_init_param(sb);
	/* Initialize base indexes for atable */
	atable_init_base(sb);

	/* vfs fields */
	vfs_sb(sb)->s_magic = TUX3_SUPER_MAGIC;
	vfs_sb(sb)->s_time_gran = TUX3_TIME_GRAN;
	vfs_sb(sb)->s_time_min = TUX3_TIME_MIN;
	vfs_sb(sb)->s_time_max = TUX3_TIME_MAX;
	vfs_sb(sb)->s_maxbytes = calc_maxbytes(sb->blocksize);
	vfs_sb(sb)->s_max_links = TUX_MAX_LINKS;

	/* Probably does not belong here (maybe metablock) */
	sb->freeinodes = MAX_INODES - be64_to_cpu(super->usedinodes);
	sb->freeblocks = sb->volblocks;
	sb->nextblock = be64_to_cpu(super->nextblock);
	sb->nextinum = TUX_NORMAL_INO;
	sb->atomdictsize = be64_to_cpu(super->atomdictsize);
	sb->atomgen = be32_to_cpu(super->atomgen);
	sb->freeatom = be32_to_cpu(super->freeatom);

	setup_roots(sb, super);
	/* This will be re-initialized if replayed logs. */
	nospc_init_balance(sb);

	/* logchain and logcount are read from super directly */
	trace("blocksize %u, blockbits %u, blockmask %08x, groupbits %u",
	      sb->blocksize, sb->blockbits, sb->blockmask, sb->groupbits);
	trace("volblocks %Lu, volmask %Lx",
	      sb->volblocks, sb->volmask);
	trace("freeblocks %Lu, freeinodes %Lu, nextblock %Lu",
	      sb->freeblocks, sb->freeinodes, sb->nextblock);
	trace("atom_dictsize %Lu, freeatom %u, atomgen %u",
	      (s64)sb->atomdictsize, sb->freeatom, sb->atomgen);
	trace("logchain %Lu, logcount %u",
	      be64_to_cpu(super->logchain), be32_to_cpu(super->logcount));
}

/* Load on-disk super block, and call setup_sb() with it */
static int load_sb(struct sb *sb)
{
	struct disksuper *super = &sb->super;
	int err;

	/* At least initialize sb, even if load is failed */
	err = init_sb(sb);
	if (err)
		return err;

	err = devio_sync(REQ_OP_READ, 0, sb_dev(sb), SB_LOC, super, SB_LEN);
	if (err)
		return err;
	if (memcmp(super->magic, TUX3_MAGIC_STR, sizeof(super->magic)))
		return -EINVAL;

	__setup_sb(sb, super);

	return 0;
}

static int tux3_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct sb *sbi = tux_sb(sb);
	/* balance can be negative */
	block_t balance = max_t(block_t, atomic64_read(&sbi->nospc.balance), 0);

	buf->f_type = TUX3_SUPER_MAGIC;
	buf->f_bsize = sbi->blocksize;
	buf->f_blocks = sbi->volblocks - nospc_min_reserve();
	buf->f_bfree = balance;
	buf->f_bavail = balance; /* FIXME: no special privilege for root yet */
	buf->f_files = MAX_INODES;
	buf->f_ffree = sbi->freeinodes;
#if 0
	buf->f_fsid.val[0] = sbi->serial_number;
	/*buf->f_fsid.val[1];*/
#endif
	buf->f_namelen = TUX_NAME_LEN;
//	buf->f_frsize = sbi->blocksize;

	return 0;
}

/* Default mount options */
const struct tux3_mount_opt tux3_default_mopt = {
	.flags		= TUX3_MOPT_BARRIER,
};

#define MOPT_SET	(1 << 0)
#define MOPT_CLEAR	(1 << 1)
#define MOPT_STRING	(1 << 2)
#define MOPT_NOSUPPORT	(1 << 3)

#define TUX3_MOUNT_OPTIONS					  	\
	OPT(Opt_barrier, "barrier", TUX3_MOPT_BARRIER, MOPT_SET), 	\
	OPT(Opt_nobarrier, "nobarrier", TUX3_MOPT_BARRIER, MOPT_CLEAR)

enum {
#define OPT(a, b, c, d)		a
	TUX3_MOUNT_OPTIONS,
#undef OPT
	Opt_err,
};

static const match_table_t tux3_tokens = {
#define OPT(a, b, c, d)		{ a, b }
	TUX3_MOUNT_OPTIONS,
#undef OPT
	/* Alternative format of options */
//	{ Opt_barrier, "barrier=%u" },
	{ Opt_err, NULL },
};

static const struct tux3_mopt_op {
	int	token;
	int	mount_opt;
	int	flags;
} tux3_mopt_ops[] = {
#define OPT(a, b, c, d)		{ a, c, d }
	TUX3_MOUNT_OPTIONS,
#undef OPT
	{ Opt_err, 0, 0 },
};

static int handle_mopt(struct sb *sbi, struct tux3_mount_opt *mopt,
		       char *opt, int token, substring_t *args)
{
	const struct tux3_mopt_op *m;
	int arg = 0;

	for (m = tux3_mopt_ops; m->token != Opt_err; m++)
		if (token == m->token)
			break;

	if (m->token == Opt_err) {
		tux3_msg(sbi, "Unrecognized mount option \"%s\" "
			 "or missing value", opt);
		return -EINVAL;
	}

	if (args->from && !(m->flags & MOPT_STRING) && match_int(args, &arg))
		return -EINVAL;

	if (m->flags & MOPT_NOSUPPORT)
		tux3_msg(sbi, "%s option not supported", opt);
	else {
		if (!args->from)
			arg = 1;
		if (m->flags & MOPT_CLEAR)
			arg = !arg;
		else if (unlikely(!(m->flags & MOPT_SET))) {
			tux3_err(sbi, "buggy handling of option %s", opt);
			return -EINVAL;
		}
		if (arg != 0)
			__TUX3_SET_MOPT(mopt, m->mount_opt);
		else
			__TUX3_CLEAR_MOPT(mopt, m->mount_opt);
	}

	return 0;
}

static int parse_options(struct sb *sbi, struct tux3_mount_opt *mopt,
			 char *options)
{
	char *p;
	substring_t args[MAX_OPT_ARGS];

	if (!options)
		return 0;

	while ((p = strsep(&options, ",")) != NULL) {
		int err, token;

		if (!*p)
			continue;
		/*
		 * Initialize args struct so we know whether arg was
		 * found; some options take optional arguments.
		 */
		args[0].to = args[0].from = NULL;
		token = match_token(p, tux3_tokens, args);
		err = handle_mopt(sbi, mopt, p, token, args);
		if (err)
			return err;
	}

	return 0;
}

static const char *token2str(int token)
{
	const struct match_token *t;

	for (t = tux3_tokens; t->token != Opt_err; t++)
		if (t->token == token && !strchr(t->pattern, '='))
			break;
	return t->pattern;
}

static int __tux3_show_options(struct seq_file *seq, struct dentry *root,
			       int all)
{
	struct sb *sbi = tux_sb(root->d_sb);
	const struct tux3_mopt_op *m;
	const unsigned int def_mopt_flags = tux3_default_mopt.flags;
	const char sep = ',';

#define SEQ_OPTS_PRINT(str, arg) seq_printf(seq, "%c" str, sep, arg)

	for (m = tux3_mopt_ops; m->token != Opt_err; m++) {
		int want_set = m->flags & MOPT_SET;

		if ((m->flags & (MOPT_SET | MOPT_CLEAR)) == 0)
			continue;
		if (!all &&
		    !(m->mount_opt & (sbi->mopt.flags ^ def_mopt_flags)))
			continue; /* skip if same as the default */
		if ((want_set &&
		     (sbi->mopt.flags & m->mount_opt) != m->mount_opt) ||
		    (!want_set && (sbi->mopt.flags & m->mount_opt)))
			continue; /* select Opt_noFoo vs Opt_Foo */

		SEQ_OPTS_PRINT("%s", token2str(m->token));
	}

	return 0;
}

#ifdef __KERNEL__
static int tux3_sync_fs(struct super_block *sb, int wait)
{
	/*
	 * FIXME: We should support "wait" parameter. wait==1 is
	 * called soon, so safe to ignore. But we should be better to
	 * submit the request early.
	 */
	if (!wait) {
		static int print_once;
		if (!print_once) {
			print_once++;
			tux3_warn(tux_sb(sb),
				  "sync_fs wait==0 is unsupported for now");
		}
		return 0;
	}
	return sync_current_delta(tux_sb(sb));
}

static void tux3_put_super(struct super_block *sb)
{
	struct sb *sbi = tux_sb(sb);

	__tux3_put_super(sbi);
	sb->s_fs_info = NULL;
	kfree(sbi);
}

/* FIXME: SB_LAZYTIME is not supported yet */
static unsigned long remove_lazytime(struct sb *sb, unsigned long flags)
{
	if (flags & SB_LAZYTIME) {
		tux3_msg(sb, "lazytime is not supported, ignored");
		flags &= ~SB_LAZYTIME;
	}
	return flags;
}

static int tux3_remount(struct super_block *sb, int *flags, char *data)
{
	struct sb *sbi = tux_sb(sb);
	struct tux3_mount_opt mopt = sbi->mopt;
	int err, remount_ro;

	/* Become read-only mount? */
	remount_ro = (*flags & SB_RDONLY) && !sb_rdonly(sb);

	err = parse_options(sbi, &mopt, data);
	if (err)
		return err;

	if (remount_ro) {
		/* Flush all before read-only */
		sync_filesystem(sb);
	}

	sbi->mopt = mopt;
	*flags = remove_lazytime(sbi, *flags);

	return 0;
}

static int tux3_show_options(struct seq_file *seq, struct dentry *root)
{
	return __tux3_show_options(seq, root, 0);
}

static const struct super_operations tux3_super_ops = {
	.alloc_inode	= tux3_alloc_inode,
	.free_inode	= tux3_free_inode,
	.dirty_inode	= tux3_dirty_inode,
	.drop_inode	= tux3_drop_inode,
	.evict_inode	= tux3_evict_inode,
	/* FIXME: we have to handle write_inode of sync (e.g. cache pressure) */
//	.write_inode	= tux3_write_inode,
#ifndef TUX3_FLUSHER_SYNC
	.writeback	= tux3_writeback,
#endif
	.sync_fs	= tux3_sync_fs,
	.put_super	= tux3_put_super,
	.statfs		= tux3_statfs,
	.remount_fs	= tux3_remount,
	.show_options	= tux3_show_options,
};

static int tux3_fill_super(struct super_block *sb, void *data, int silent)
{
	struct sb *sbi;
	struct replay *rp = NULL;
	int err, blocksize;

	sbi = kzalloc(sizeof(struct sb), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	sbi->vfs_sb = sb;
	sb->s_fs_info = sbi;
	/*
	 * FIXME: atime can insert inode into dirty list unexpectedly.
	 * For now, doesn't support and disable atime.
	 */
	sb->s_flags |= SB_NOATIME;
	sb->s_op = &tux3_super_ops;
	/* Set default mount options */
	sbi->mopt = tux3_default_mopt;
	sb->s_flags = remove_lazytime(sbi, sb->s_flags);

	err = parse_options(sbi, &sbi->mopt, data);
	if (err)
		goto error_free;

	err = -EIO;
	blocksize = sb_min_blocksize(sb, BLOCK_SIZE);
	if (!blocksize) {
		if (!silent)
			printk(KERN_ERR "TUX3: unable to set blocksize\n");
		goto error_free;
	}

	/* Initialize and load sbi */
	err = load_sb(sbi);
	if (err) {
		if (!silent) {
			if (err == -EINVAL)
				tux3_err(sbi, "invalid superblock [%Lx]",
				     be64_to_cpup((__be64 *)sbi->super.magic));
			else
				tux3_err(sbi, "unable to read superblock");
		}
		goto error;
	}

	if (sbi->blocksize != blocksize) {
		if (!sb_set_blocksize(sb, sbi->blocksize)) {
			tux3_err(sbi, "blocksize too small for device");
			goto error;
		}
	}
	tux3_dbg("s_blocksize %lu", sb->s_blocksize);

	rp = tux3_init_fs(sbi);
	if (IS_ERR(rp)) {
		err = PTR_ERR(rp);
		goto error;
	}

	err = replay_stage3(rp, 1);
	if (err) {
		rp = NULL;
		goto error;
	}

	sb->s_root = d_make_root(sbi->rootdir);
	sbi->rootdir = NULL;	/* vfs takes care rootdir inode */
	if (!sb->s_root) {
		err = -ENOMEM;
		goto error;
	}

	return 0;

error:
	if (!IS_ERR_OR_NULL(rp))
		replay_stage3(rp, 0);
	__tux3_put_super(sbi);
error_free:
	kfree(sbi);

	return err;
}

static struct dentry *tux3_mount(struct file_system_type *fs_type, int flags,
	const char *dev_name, void *data)
{
	return mount_bdev(fs_type, flags, dev_name, data, tux3_fill_super);
}

static struct file_system_type tux3_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "tux3",
	.fs_flags	= FS_REQUIRES_DEV,
	.mount		= tux3_mount,
	.kill_sb	= kill_block_super,
};

static int __init init_tux3(void)
{
	int err;

	err = tux3_init_inodecache();
	if (err)
		goto error;

	err = tux3_init_hole_cache();
	if (err)
		goto error_hole;

	err = tux3_init_idefer_cache();
	if (err)
		goto error_idefer;

	err = register_filesystem(&tux3_fs_type);
	if (err)
		goto error_fs;

	return 0;

error_fs:
	tux3_destroy_idefer_cache();
error_idefer:
	tux3_destroy_hole_cache();
error_hole:
	tux3_destroy_inodecache();
error:
	return err;
}

static void __exit exit_tux3(void)
{
	unregister_filesystem(&tux3_fs_type);
	tux3_destroy_idefer_cache();
	tux3_destroy_hole_cache();
	tux3_destroy_inodecache();
}

module_init(init_tux3);
module_exit(exit_tux3);
MODULE_DESCRIPTION("Tux3 Filesystem");
MODULE_AUTHOR("Daniel Phillips, OGAWA Hirofumi");
MODULE_LICENSE("GPL");
MODULE_ALIAS_FS("tux3");
#endif /* __KERNEL__ */
