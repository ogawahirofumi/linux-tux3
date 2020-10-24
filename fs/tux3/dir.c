/*
 * Directory entry handling lifted from Ext2, blush.
 *
 * Portions (c) Daniel Phillips 2008-2013
 * Copyright (c) 2008-2014 OGAWA Hirofumi
 */

/*
 * from linux/include/linux/ext2_fs.h and linux/fs/ext2/dir.c
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 * from linux/include/linux/minix_fs.h
 *
 * Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  ext2 directory handling functions
 *
 *  Big-endian to little-endian byte-swapping/bitmaps by
 *        David S. Miller (davem@caip.rutgers.edu), 1995
 *
 * All code that works with directory layout had been switched to pagecache
 * and moved here. AV
 * Copied to tux3 and switched back to buffers, Daniel Phillips 2008
 */

#include "tux3.h"

#ifndef trace
#define trace trace_off
#endif

#define TUX_DIR_ALIGN		sizeof(inum_t)
#define TUX_DIR_HEAD		(offsetof(struct tux3_dirent, name))
#define TUX_REC_LEN(name_len)	ALIGN((name_len) + TUX_DIR_HEAD, TUX_DIR_ALIGN)
#define TUX_MAX_REC_LEN		((1 << 16) - 1)

static inline unsigned tux_rec_len_from_disk(__be16 dlen)
{
	unsigned len = be16_to_cpu(dlen);
#if (PAGE_SIZE >= 65536)
	if (len == TUX_MAX_REC_LEN)
		return 1 << 16;
#endif
	return len;
}

static inline __be16 tux_rec_len_to_disk(unsigned len)
{
#if (PAGE_SIZE >= 65536)
	if (len == (1 << 16))
		return cpu_to_be16(TUX_MAX_REC_LEN);
	else
		BUG_ON(len > (1 << 16));
#endif
	return cpu_to_be16(len);
}

static inline int is_deleted(struct tux3_dirent *entry)
{
	return !entry->name_len; /* ext2 uses !inum for this */
}

static inline int tux_match(struct tux3_dirent *entry, const char *const name,
			    unsigned len)
{
	if (len != entry->name_len)
		return 0;
	if (is_deleted(entry))
		return 0;
	return !memcmp(name, entry->name, len);
}

static inline struct tux3_dirent *next_entry(struct tux3_dirent *entry)
{
	return (void *)entry + tux_rec_len_from_disk(entry->rec_len);
}

#define tux_zero_len_error(dir, block)					\
	tux3_fs_error(tux_sb((dir)->i_sb),				\
		      "zero length entry at inum %Lu, block %Lu",	\
		      tux_inode(dir)->inum, block)

void tux_set_entry(struct buffer_head *buffer, struct tux3_dirent *entry,
		   inum_t inum, umode_t mode)
{
	entry->inum = cpu_to_be64(inum);
	entry->type = fs_umode_to_ftype(mode);
	mark_buffer_dirty_non(buffer);
	blockput(buffer);
}

/*
 * NOTE: For now, we don't have ".." though, we shouldn't use this for
 * "..". rename() shouldn't update ->mtime for ".." usually.
 */
void tux_update_dirent(struct inode *dir, struct buffer_head *buffer,
		       struct tux3_dirent *entry, struct inode *inode)
{
	tux_set_entry(buffer, entry, tux_inode(inode)->inum, inode->i_mode);

	tux3_iattrdirty(dir);
	dir->i_mtime = dir->i_ctime = current_time(dir);
	tux3_mark_inode_dirty(dir);
}

loff_t tux_alloc_entry(struct inode *dir, const char *name, unsigned len,
		       loff_t *size, struct buffer_head **hold)
{
	unsigned delta = tux3_get_current_delta();
	struct sb *sb = tux_sb(dir->i_sb);
	struct tux3_dirent *entry;
	struct buffer_head *buffer, *clone;
	unsigned reclen = TUX_REC_LEN(len), offset;
	unsigned name_len, rec_len;
	unsigned blocksize = sb->blocksize;
	block_t block, blocks = *size >> sb->blockbits;
	void *olddata;

	for (block = 0; block < blocks; block++) {
		buffer = blockread(mapping(dir), block);
		if (!buffer)
			return -EIO;
		entry = bufdata(buffer);
		struct tux3_dirent *limit = bufdata(buffer) + blocksize - reclen;
		while (entry <= limit) {
			if (entry->rec_len == 0) {
				blockput(buffer);
				tux_zero_len_error(dir, block);
				return -EIO;
			}
			name_len = TUX_REC_LEN(entry->name_len);
			rec_len = tux_rec_len_from_disk(entry->rec_len);
			if (is_deleted(entry) && rec_len >= reclen)
				goto create;
			if (rec_len >= name_len + reclen)
				goto create;
			entry = (void *)entry + rec_len;
		}
		blockput(buffer);
	}
	entry = NULL;
	buffer = blockget(mapping(dir), block);
	assert(!buffer_dirty(buffer));

create:
	/*
	 * The directory is protected by inode_lock.
	 * blockdirty() should never return -EAGAIN.
	 */
	olddata = bufdata(buffer);
	clone = blockdirty(buffer, delta);
	if (IS_ERR(clone)) {
		assert(PTR_ERR(clone) != -EAGAIN);
		blockput(buffer);
		return PTR_ERR(clone);
	}
	if (!entry) {
		/* Expanding the directory size. Initialize block. */
		entry = bufdata(clone);
		memset(entry, 0, blocksize);
		entry->rec_len = tux_rec_len_to_disk(blocksize);
		assert(is_deleted(entry));

		*size += blocksize;
	} else {
		entry = ptr_redirect(entry, olddata, bufdata(clone));

		if (!is_deleted(entry)) {
			struct tux3_dirent *newent = (void *)entry + name_len;
			unsigned rest_rec_len = rec_len - name_len;
			newent->rec_len = tux_rec_len_to_disk(rest_rec_len);
			entry->rec_len = tux_rec_len_to_disk(name_len);
			entry = newent;
		}
	}

	entry->name_len = len;
	memcpy(entry->name, name, len);
	offset = (void *)entry - bufdata(clone);

	*hold = clone;
	return (block << sb->blockbits) + offset; /* only for xattr create */
}

struct inode *__tux_create_dirent(struct inode *dir, const struct qstr *qstr,
				  struct inode *inode, struct tux_iattr *iattr)
{
	struct sb *sb = tux_sb(dir->i_sb);
	const char *name = (const char *)qstr->name;
	unsigned len = qstr->len;
	struct buffer_head *buffer;
	struct tux3_dirent *entry;
	loff_t i_size, where;
	inum_t inum;
	int err, err2;

	/* Holding inode_lock(dir), so no i_size_read() */
	i_size = dir->i_size;
	where = tux_alloc_entry(dir, name, len, &i_size, &buffer);
	if (where < 0)
		return ERR_PTR(where);
	entry = bufdata(buffer) + (where & sb->blockmask);

	if (inode == NULL) {
		inode = tux_create_inode(dir, where, iattr);
		if (IS_ERR(inode)) {
			err = PTR_ERR(inode);
			goto error;
		}
	}
	inum = tux_inode(inode)->inum;

	/* This releases buffer */
	tux_set_entry(buffer, entry, inum, inode->i_mode);

	tux3_iattrdirty(dir);
	if (dir->i_size != i_size)
		i_size_write(dir, i_size);

	dir->i_mtime = dir->i_ctime = current_time(dir);
	tux3_mark_inode_dirty(dir);

	return inode;

error:
	err2 = tux_delete_entry(dir, buffer, entry);
	if (err2)
		tux3_fs_error(sb, "Failed to recover dir entry (err %d)", err2);

	return ERR_PTR(err);
}

struct tux3_dirent *tux_find_entry(struct inode *dir, const char *name,
				   unsigned len, struct buffer_head **result,
				   loff_t size)
{
	struct sb *sb = tux_sb(dir->i_sb);
	unsigned reclen = TUX_REC_LEN(len);
	block_t block, blocks = size >> sb->blockbits;
	int err = -ENOENT;

	for (block = 0; block < blocks; block++) {
		struct buffer_head *buffer = blockread(mapping(dir), block);
		if (!buffer) {
			err = -EIO; // need ERR_PTR for blockread!!!
			goto error;
		}
		struct tux3_dirent *entry = bufdata(buffer);
		struct tux3_dirent *limit = (void *)entry + sb->blocksize - reclen;
		while (entry <= limit) {
			if (entry->rec_len == 0) {
				blockput(buffer);
				tux_zero_len_error(dir, block);
				err = -EIO;
				goto error;
			}
			if (tux_match(entry, name, len)) {
				*result = buffer;
				return entry;
			}
			entry = next_entry(entry);
		}
		blockput(buffer);
	}
error:
	*result = NULL;		/* for debug */
	return ERR_PTR(err);
}

struct tux3_dirent *tux_find_dirent(struct inode *dir, const struct qstr *qstr,
				    struct buffer_head **result)
{
	/* Holding inode_lock(_shared)(dir), so no i_size_read() */
	return tux_find_entry(dir, (const char *)qstr->name, qstr->len,
			      result, dir->i_size);
}

/*
 * Return 0 if the directory entry is OK, and 1 if there is a problem
 */
static int __check_dir_entry(const char *func, int line, struct inode *dir,
			     struct buffer_head *buffer,
			     struct tux3_dirent *entry)
{
	struct sb *sb = tux_sb(dir->i_sb);
	const char *error_msg = NULL;
	const void *base = bufdata(buffer);
	const int off = (void *)entry - base;
	const int rlen = tux_rec_len_from_disk(entry->rec_len);

	if (unlikely(rlen < TUX_REC_LEN(1)))
		error_msg = "rec_len is smaller than minimal";
	else if (unlikely(rlen & (TUX_DIR_ALIGN - 1)))
		error_msg = "rec_len alignment error";
	else if (unlikely(rlen < TUX_REC_LEN(entry->name_len)))
		error_msg = "rec_len is too small for name_len";
	else if (unlikely(off + rlen > sb->blocksize))
		error_msg = "directory entry across range";
	else
		return 0;

	__tux3_err(sb, func, line,
		   "bad entry: %s: inum %Lu, block %Lu, off %d, rec_len %d",
		   error_msg, tux_inode(dir)->inum, bufindex(buffer),
		   off, rlen);

	return 1;
}
#define check_dir_entry(d, b, e)		\
	__check_dir_entry(__func__, __LINE__, d, b, e)

static unsigned tux_validate_entry(void *base, unsigned offset)
{
	struct tux3_dirent *entry = base + offset;
	struct tux3_dirent *p = base;
	while (p < entry && p->rec_len)
		p = next_entry(p);
	return (void *)p - base;
}

static bool tux3_dir_emit_dots(struct file *file, struct dir_context *ctx)
{
	if (ctx->pos == 0) {
		inum_t inum = tux_inode(file_inode(file))->inum;
		if (!dir_emit(ctx, ".", 1, inum, DT_DIR))
			return false;
		ctx->pos = 1;
	}
	if (ctx->pos == 1) {
		inum_t inum = tux_inode(file_inode(file))->parent_inum;
		if (!dir_emit(ctx, "..", 2, inum, DT_DIR))
			return false;
		ctx->pos = 2;
	}
	return true;
}

int tux_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *dir = file_inode(file);
	bool need_revalidate = !inode_eq_iversion(dir, file->f_version);
	struct sb *sb = tux_sb(dir->i_sb);
	unsigned blockbits = sb->blockbits;
	block_t block, blocks = dir->i_size >> blockbits;
	unsigned offset;

	assert(!(dir->i_size & sb->blockmask));

	/* Handle "." and ".." */
	if (!tux3_dir_emit_dots(file, ctx))
		return 0;

	/* pos == 2 is the start of real entries in dir blocks */
	if (ctx->pos == 2)
		offset = 0;
	else {
		/* Clearly invalid offset */
		if (unlikely(ctx->pos & (TUX_DIR_ALIGN - 1)))
			return -ENOENT;
		offset = ctx->pos & sb->blockmask;
	}

	for (block = ctx->pos >> blockbits; block < blocks; block++) {
		struct buffer_head *buffer = blockread(mapping(dir), block);
		if (!buffer)
			return -EIO;
		void *base = bufdata(buffer);
		if (need_revalidate) {
			if (offset) {
				offset = tux_validate_entry(base, offset);
				ctx->pos = (block << blockbits) + offset;
				/* Adjust pos for fake "." and ".." */
				ctx->pos = max_t(loff_t, ctx->pos, 2);
			}
			file->f_version = inode_query_iversion(dir);
			need_revalidate = false;
		}
		struct tux3_dirent *limit = base + sb->blocksize - TUX_REC_LEN(1);
		struct tux3_dirent *entry;
		for (entry = base + offset; entry <= limit; entry = next_entry(entry)) {
			if (check_dir_entry(dir, buffer, entry)) {
				/* On error, skip to next block */
				ctx->pos = (ctx->pos | sb->blockmask) + 1;
				break;
			}
			if (!is_deleted(entry)) {
				unsigned type = fs_ftype_to_dtype(entry->type);
				if (!dir_emit(ctx, entry->name, entry->name_len,
					      be64_to_cpu(entry->inum), type)) {
					blockput(buffer);
					return 0;
				}
			}
			/* Adjust pos for fake "." and ".." */
			if (ctx->pos == 2)
				ctx->pos = tux_rec_len_from_disk(entry->rec_len);
			else
				ctx->pos += tux_rec_len_from_disk(entry->rec_len);
		}
		blockput(buffer);
		offset = 0;

		if (ctx->pos < dir->i_size) {
			if (!dir_relax_shared(dir))
				return 0;
		}
	}
	return 0;
}

int tux_delete_entry(struct inode *dir, struct buffer_head *buffer,
		     struct tux3_dirent *entry)
{
	unsigned delta = tux3_get_current_delta();
	struct tux3_dirent *prev = NULL, *this = bufdata(buffer);
	struct buffer_head *clone;
	void *olddata;

	while ((char *)this < (char *)entry) {
		if (this->rec_len == 0) {
			blockput(buffer);
			tux_zero_len_error(dir, bufindex(buffer));
			return -EIO;
		}
		prev = this;
		this = next_entry(this);
	}

	/*
	 * The directory is protected by inode_lock.
	 * blockdirty() should never return -EAGAIN.
	 */
	olddata = bufdata(buffer);
	clone = blockdirty(buffer, delta);
	if (IS_ERR(clone)) {
		assert(PTR_ERR(clone) != -EAGAIN);
		blockput(buffer);
		return PTR_ERR(clone);
	}
	entry = ptr_redirect(entry, olddata, bufdata(clone));
	prev = ptr_redirect(prev, olddata, bufdata(clone));

	if (prev)
		prev->rec_len = tux_rec_len_to_disk((void *)next_entry(entry) - (void *)prev);
	memset(entry->name, 0, entry->name_len);
	entry->name_len = entry->type = 0;
	entry->inum = 0;

	mark_buffer_dirty_non(clone);
	blockput(clone);

	return 0;
}

int tux_delete_dirent(struct inode *dir, struct buffer_head *buffer,
		      struct tux3_dirent *entry)
{
	int err;

	err = tux_delete_entry(dir, buffer, entry); /* this releases buffer */
	if (!err) {
		tux3_iattrdirty(dir);
		dir->i_ctime = dir->i_mtime = current_time(dir);
		tux3_mark_inode_dirty(dir);
	}

	return err;
}

int tux_dir_is_empty(struct inode *dir)
{
	struct sb *sb = tux_sb(dir->i_sb);
	block_t block, blocks = dir->i_size >> sb->blockbits;
	__be64 self = cpu_to_be64(tux_inode(dir)->inum);
	struct buffer_head *buffer;

	for (block = 0; block < blocks; block++) {
		buffer = blockread(mapping(dir), block);
		if (!buffer)
			return -EIO;

		struct tux3_dirent *entry = bufdata(buffer);
		struct tux3_dirent *limit = bufdata(buffer) + sb->blocksize - TUX_REC_LEN(1);
		for (; entry <= limit; entry = next_entry(entry)) {
			if (!entry->rec_len) {
				blockput(buffer);
				tux_zero_len_error(dir, block);
				return -EIO;
			}
			if (is_deleted(entry))
				continue;
			if (entry->name[0] != '.')
				goto not_empty;
			if (entry->name_len > 2)
				goto not_empty;
			if (entry->name_len < 2) {
				if (entry->inum != self)
					goto not_empty;
			} else if (entry->name[1] != '.')
				goto not_empty;
		}
		blockput(buffer);
	}
	return 0;
not_empty:
	blockput(buffer);
	return -ENOTEMPTY;
}
