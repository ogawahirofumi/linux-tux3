/*
 * Copied some vfs library functions to add change_{begin,end} and
 * tux3_iattrdirty().
 *
 * We should check the update of original functions, and sync with it.
 */

#include <linux/splice.h>
#include <linux/aio.h>		/* for kiocb */

/*
 * Almost copy of generic_file_write_iter() (added changed_begin/end,
 * tux3_iattrdirty()).
 */
static ssize_t tux3_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file->f_mapping->host;
	struct sb *sb = tux_sb(inode->i_sb);
	ssize_t ret;

	inode_lock(inode);
	/* For each ->write_end() calls change_end(). */
	if (change_begin(sb, 1)) {
		inode_unlock(inode);
		return -ENOSPC;
	}
	ret = generic_write_checks(iocb, from);
	if (ret > 0) {
		/* FIXME: file_update_time() in this can be race with mmap */
		ret = __generic_file_write_iter(iocb, from);
	}
	if (change_active())
		change_end(sb);
	inode_unlock(inode);

	if (ret > 0)
		ret = generic_write_sync(iocb, ret);
	return ret;
}
