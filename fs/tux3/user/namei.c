/*
 * Tux3 versioning filesystem in user space
 *
 * Original copyright (c) 2008 Daniel Phillips <phillips@phunq.net>
 * Original copyright (c) 2012 OGAWA Hirofumi <hirofumi@mail.parknet.co.jp>
 * Licensed under the GPL version 3
 *
 * By contributing changes to this file you grant the original copyright holder
 * the right to distribute those changes under any license.
 */

#include "tux3user.h"

static void d_instantiate_new(struct dentry *dentry, struct inode *inode)
{
	d_instantiate(dentry, inode);
	unlock_new_inode(inode);
}

#include "../namei.c"

static int tuxlookup(struct inode *dir, struct dentry *dentry)
{
	struct dentry *result;

	result = tux3_lookup(dir, dentry, 0);
	if (result && IS_ERR(result))
		return PTR_ERR(result);
	assert(result == NULL);

	if (!d_inode(dentry))
		return -ENOENT;

	return 0;
}

struct inode *tuxopen(struct inode *dir, const char *name, unsigned len)
{
	struct dentry dentry = {
		.d_name.name = (unsigned char *)name,
		.d_name.len = len,
	};
	int err;

	err = tuxlookup(dir, &dentry);
	if (err)
		return ERR_PTR(err);

	return d_inode(&dentry);
}

static int tux_check_exist(struct inode *dir, struct qstr *qstr)
{
	struct buffer_head *buffer;
	struct tux3_dirent *entry;

	entry = tux_find_dirent(dir, qstr, &buffer);
	if (!IS_ERR(entry)) {
		blockput(buffer);
		return -EEXIST;
	}
	if (PTR_ERR(entry) != -ENOENT)
		return PTR_ERR(entry);

	return 0;
}

struct inode *__tuxmknod(struct inode *dir, const char *name, unsigned len,
			 struct tux_iattr *iattr)
{
	struct dentry dentry = {
		.d_name.name = (unsigned char *)name,
		.d_name.len = len,
	};
	int err;

	if (S_ISDIR(iattr->mode) && dir->i_nlink >= dir->i_sb->s_max_links)
		return ERR_PTR(-EMLINK);

	err = __tux3_mknod(dir, &dentry, iattr);
	if (err)
		return ERR_PTR(err);

	return d_inode(&dentry);
}

struct inode *tuxmknod(struct inode *dir, const char *name, unsigned len,
		       struct tux_iattr *iattr)
{
	struct qstr qstr = {
		.name = (unsigned char *)name,
		.len = len,
	};
	int err;

	/*
	 * FIXME: we can find space with existent check
	 */

	err = tux_check_exist(dir, &qstr);
	if (err)
		return ERR_PTR(err);

	return __tuxmknod(dir, name, len, iattr);
}

struct inode *tuxcreate(struct inode *dir, const char *name, unsigned len,
			struct tux_iattr *iattr)
{
	/* If a file exists, we should fall back to open? */
	return tuxmknod(dir, name, len, iattr);
}

struct inode *__tuxlink(struct inode *src_inode, struct inode *dir,
			const char *dstname, unsigned dstlen)
{
	struct dentry src = {
		.d_inode = src_inode,
	};
	struct dentry dst = {
		.d_name.name = (unsigned char *)dstname,
		.d_name.len = dstlen,
	};
	int err;

	if (src_inode->i_nlink >= dir->i_sb->s_max_links)
		return ERR_PTR(-EMLINK);

	err = tux3_link(&src, dir, &dst);
	if (err)
		return ERR_PTR(err);
	assert(d_inode(&dst) == src_inode);
	return d_inode(&dst);
}

int tuxlink(struct inode *dir, const char *srcname, unsigned srclen,
	    const char *dstname, unsigned dstlen)
{
	struct inode *src_inode;
	int err;

	src_inode = tuxopen(dir, srcname, srclen);
	if (IS_ERR(src_inode))
		return PTR_ERR(src_inode);

	if (S_ISDIR(src_inode->i_mode)) {
		err = -EPERM;
		goto error_src;
	}
	/* Orphaned inode. We shouldn't grab this. */
	if (src_inode->i_nlink == 0) {
		err = -ENOENT;
		goto error_src;
	}

	/*
	 * FIXME: we can find space with existent check
	 */

	struct qstr dststr = {
		.name = (unsigned char *)dstname,
		.len = dstlen,
	};
	err = tux_check_exist(dir, &dststr);
	if (err)
		goto error_src;

	struct inode *inode = __tuxlink(src_inode, dir, dstname, dstlen);
	if (IS_ERR(inode))
		err = PTR_ERR(inode);
	else
		iput(inode);
error_src:
	iput(src_inode);

	return err;
}

int tuxreadlink(struct inode *dir, const char *name, unsigned len,
		void *buf, unsigned bufsize)
{
	struct inode *inode;
	int res;

	inode = tuxopen(dir, name, len);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	res = -EINVAL;
	if (S_ISLNK(inode->i_mode))
		res = page_readlink(inode, buf, bufsize);
	iput(inode);

	return res;
}

struct inode *__tuxsymlink(struct inode *dir, const char *name, unsigned len,
			   struct tux_iattr *iattr, const char *symname)
{
	struct dentry dentry = {
		.d_name.name = (unsigned char *)name,
		.d_name.len = len,
	};
	int err;

	iattr->mode = S_IFLNK | S_IRWXUGO;
	err = __tux3_symlink(dir, &dentry, iattr, symname);
	if (err)
		return ERR_PTR(err);
	return d_inode(&dentry);
}

int tuxsymlink(struct inode *dir, const char *name, unsigned len,
	       struct tux_iattr *iattr, const char *symname)
{
	struct qstr qstr = {
		.name = (unsigned char *)name,
		.len = len,
	};
	struct inode *inode;
	int err;

	if (strlen(symname) == 0)
		return -ENOENT;

	/*
	 * FIXME: we can find space with existent check
	 */

	err = tux_check_exist(dir, &qstr);
	if (err)
		return err;

	inode = __tuxsymlink(dir, name, len, iattr, symname);
	err = PTR_ERR(inode);
	if (!IS_ERR(inode)) {
		iput(inode);
		err = 0;
	}

	return err;
}

int tuxunlink(struct inode *dir, const char *name, unsigned len)
{
	struct dentry dentry = {
		.d_name.name = (unsigned char *)name,
		.d_name.len = len,
	};
	int err;

	/*
	 * FIXME: we can cache dirent position by tuxlookup(), and
	 * tux3_unlink() can use it.
	 */

	err = tuxlookup(dir, &dentry);
	if (err)
		return err;

	err = tux3_unlink(dir, &dentry);

	/* This iput() will schedule deletion if i_nlink == 0 && i_count == 1 */
	iput(d_inode(&dentry));

	return err;
}

int tuxrmdir(struct inode *dir, const char *name, unsigned len)
{
	struct dentry dentry = {
		.d_name.name = (unsigned char *)name,
		.d_name.len = len,
	};
	int err;

	/*
	 * FIXME: we can cache dirent position by tuxlookup(), and
	 * tux3_unlink() can use it.
	 */

	err = tuxlookup(dir, &dentry);
	if (err)
		return err;

	err = -ENOTDIR;
	if (S_ISDIR(d_inode(&dentry)->i_mode))
		err = tux3_rmdir(dir, &dentry);

	/* This iput() will schedule deletion if i_nlink == 0 && i_count == 1 */
	iput(d_inode(&dentry));

	return err;
}

int tuxrename(struct inode *old_dir, const char *old_name, unsigned old_len,
	      struct inode *new_dir, const char *new_name, unsigned new_len,
	      unsigned int flags)
{
	struct dentry old = {
		.d_name.name = (unsigned char *)old_name,
		.d_name.len = old_len,
	};
	struct dentry new = {
		.d_name.name = (unsigned char *)new_name,
		.d_name.len = new_len,
	};
	int err, is_dir, new_is_dir;

	/*
	 * FIXME: we can cache dirent position by tuxlookup(), and
	 * tux3_rename() can use it.
	 */

	err = tuxlookup(old_dir, &old);
	if (err)
		return err;

	err = tuxlookup(new_dir, &new);
	if (err && err != -ENOENT)
		goto error_old;

	/* FIXME: check is not enough */
	err = 0;
	if (d_inode(&old) == d_inode(&new))
		goto out;
	is_dir = S_ISDIR(d_inode(&old)->i_mode);
	new_is_dir = d_inode(&new) && S_ISDIR(d_inode(&new)->i_mode);
	if (d_inode(&new)) {
		if (is_dir != new_is_dir) {
			err = is_dir ? -ENOTDIR : -EISDIR;
			goto out;
		}
	} else if (new_dir != old_dir) {
		unsigned max_links = new_dir->i_sb->s_max_links;
		if (is_dir && !new_is_dir && new_dir->i_nlink >= max_links) {
			err = -EMLINK;
			goto out;
		}
	}

	err = tux3_rename(&init_user_ns, old_dir, &old, new_dir, &new, flags);
out:
	if (d_inode(&new))
		iput(d_inode(&new));
error_old:
	iput(d_inode(&old));

	return err;
}
