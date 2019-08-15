/*
 * Tux3 versioning filesystem in user space
 *
 * Original copyright (c) 2008 Daniel Phillips <phillips@phunq.net>
 * Licensed under the GPL version 3
 *
 * By contributing changes to this file you grant the original copyright holder
 * the right to distribute those changes under any license.
 */

#include "tux3user.h"
#include <libklib/seq_file.h>

#ifndef trace
#define trace trace_off
#endif

#include "../super.c"

struct inode *__alloc_inode(struct super_block *sb)
{
	return tux3_alloc_inode(sb);
}

void __destroy_inode(struct inode *inode)
{
	tux3_destroy_inode(inode);
}

void put_super(struct sb *sb)
{
	vfs_sb(sb)->s_flags &= ~SB_ACTIVE;

	__tux3_put_super(sb);
	inode_leak_check();
}

/* Initialize and setup sb by on-disk super block. */
int setup_sb(struct sb *sb, struct disksuper *super)
{
	int err;

	err = init_sb(sb);
	if (err)
		return err;

	__setup_sb(sb, super);

	/* This setup doesn't add SB_ACTIVE. */

	return 0;
}

struct replay *__load_fs(struct sb *sb)
{
	int err = load_sb(sb);
	if (err)
		return ERR_PTR(err);

	sb_dev(sb)->bits = sb->blockbits;
	set_blocksize(sb->blocksize);

	struct replay *rp = tux3_init_fs(sb);
	if (!IS_ERR(rp)) {
		/* Now, sb setup was done. */
		vfs_sb(sb)->s_flags |= SB_ACTIVE;
	}
	return rp;
}

int load_fs(struct sb *sb, int apply_orphan)
{
	struct replay *rp;
	int err;

	rp = __load_fs(sb);
	if (IS_ERR(rp)) {
		err = PTR_ERR(rp);
		goto error;
	}

	err = replay_stage3(rp, apply_orphan);
	if (err) {
		rp = NULL;
		goto error;
	}

	return 0;

error:
	if (!IS_ERR_OR_NULL(rp))
		replay_stage3(rp, 0);
	put_super(sb);
	return err;
}

/* Clear first and last block to get rid of other magic */
static int clear_other_magic(struct sb *sb)
{
	struct {
		loff_t loc;
		unsigned len;
	} area[] = {
		{ 0, SB_LOC },
		{ (sb->volblocks - 1) << sb->blockbits, sb->blocksize },
	};
	void *data;
	unsigned maxlen = 0;
	int err;

	for (int i = 0; i < ARRAY_SIZE(area); i++)
		maxlen = max(maxlen, area[i].len);

	data = malloc(maxlen);
	if (!data)
		return -ENOMEM;
	memset(data, 0, maxlen);

	for (int i = 0; i < ARRAY_SIZE(area); i++) {
		err = devio_sync(REQ_OP_WRITE, 0, sb_dev(sb), area[i].loc, data,
				 area[i].len);
		if (err)
			break;
	}

	free(data);

	return err;
}

static int reserve_superblock(struct sb *sb)
{
	/* Always 8K regardless of blocksize */
	struct block_segment seg = {
		.block = 0,
		.count = 1 << (sb->blockbits > 13 ? 0 : 13 - sb->blockbits),
	};
	int err;

	trace("reserve superblock");

	/* Reserve blocks from 0 to 8KB */
	err = balloc_use(sb, &seg, 1);
	if (err < 0)
		return err;

	log_balloc(sb, seg.block, seg.count);
	trace("reserve %Lx", seg.block);

	return 0;
}

static int do_mkfs_tux3(struct sb *sb)
{
	struct inode *inode;
	int err;

	err = clear_other_magic(sb);
	if (err)
		return err;

	change_begin_atomic(sb);

	trace("create bitmap");
	inode = create_internal_inode(sb, TUX_BITMAP_INO, NULL);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto error_change_end;
	}
	sb->bitmap = inode;

	inode = create_internal_inode(sb, TUX_COUNTMAP_INO, NULL);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto error_change_end;
	}
	sb->countmap = inode;

	change_end_atomic(sb);

	/* Set fake backend mark to modify backend objects. */
	tux3_start_backend(sb);
	err = reserve_superblock(sb);
	tux3_end_backend();
	if (err)
		goto error;

	change_begin_atomic(sb);
#if 0
	trace("create version table");
	inode = create_internal_inode(sb, TUX_VTABLE_INO, NULL);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto error_change_end;
	}
	sb->vtable = inode;
#endif
	trace("create atom dictionary");
	inode = create_internal_inode(sb, TUX_ATABLE_INO, NULL);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto error_change_end;
	}
	sb->atable = inode;

	trace("create root directory");
	struct tux_iattr root_iattr = { .mode = S_IFDIR | 0755, };
	inode = create_internal_inode(sb, TUX_ROOTDIR_INO, &root_iattr);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto error_change_end;
	}
	sb->rootdir = inode;

	change_end_atomic(sb);

	return 0;

error_change_end:
	change_end_atomic(sb);
error:
	/* Caller have responsibility to cleanup inodes. */
	return err;
}

int __mkfs_tux3(struct sb *sb)
{
	int err;

	err = setup_sb(sb, &sb->super);
	if (err)
		return err;

	err = set_blocksize(sb->blocksize);
	if (err)
		return err;

	sb->volmap = tux_new_volmap(sb);
	if (!sb->volmap)
		return -ENOMEM;

	sb->logmap = tux_new_logmap(sb);
	if (!sb->logmap)
		return -ENOMEM;

	err = do_mkfs_tux3(sb);
	if (err)
		return err;

	/* Now, sb setup was done. */
	vfs_sb(sb)->s_flags |= SB_ACTIVE;

	return 0;
}

int mkfs_tux3(struct sb *sb)
{
	int err;

	err = __mkfs_tux3(sb);
	if (err)
		goto error;

	err = sync_super(sb);
	if (err)
		goto error_sync;

	show_buffers(mapping(sb->bitmap));
	show_buffers(mapping(sb->rootdir));
	show_buffers(sb->volmap->map);

	return 0;

error_sync:
	; /* FIXME: error */
error:
	return err;
}

int setup_mount_options(struct sb *sb, void *data)
{
	return parse_options(sb, &sb->mopt, data);
}

ssize_t get_mount_options(struct sb *sb, char *buf, size_t size, int all)
{
	struct dentry dummy = {
		.d_sb = vfs_sb(sb),
	};
	struct seq_file seq = {
		.buf = buf,
		.size = size,
	};
	int err;

	err = __tux3_show_options(&seq, &dummy, all);
	if (seq_has_overflowed(&seq))
		err = -EOVERFLOW;
	if (err)
		return err;

	/* nul terminate for convenience. */
	if (seq.count + 1 > size)
		return -EOVERFLOW;
	buf[seq.count] = '\0';
	return seq.count + 1;
}

int tux3_get_kstatfs(struct sb *sb, struct kstatfs *kstatfs)
{
	struct dentry dentry = {
		.d_sb = vfs_sb(sb),
	};
	int err;

	memset(kstatfs, 0, sizeof(*kstatfs));
	err = tux3_statfs(&dentry, kstatfs);
	if (!err) {
		/* FIXME: need to fill mount flags? */
		kstatfs->f_flags = 0;
		/* tux3_statvfs() may not fill */
		if (kstatfs->f_frsize == 0)
			kstatfs->f_frsize = kstatfs->f_bsize;
	}
	return err;
}

#include "aligncheck.h"

int tux3_init_mem(unsigned poolsize, int debug)
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

	init_buffer_params(poolsize, debug);
	init_alignment_check();

	return 0;

error_idefer:
	tux3_destroy_hole_cache();
error_hole:
	tux3_destroy_inodecache();
error:
	return err;
}

void tux3_exit_mem(void)
{
	tux3_destroy_idefer_cache();
	tux3_destroy_hole_cache();
	tux3_destroy_inodecache();
}
