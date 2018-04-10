#include "tux3user.h"
#include "test.h"

#ifndef trace
#define trace trace_off
#endif

#include "../dir.c"

static void clean_main(struct sb *sb, struct inode *dir)
{
	invalidate_buffers(dir->map);
	rapid_free_inode(dir);
	put_super(sb);
	tux3_exit_mem();
}

/* Test basic dir operations */
static void test01(struct sb *sb, struct inode *dir)
{
	struct buffer_head *buffer;
	struct tux3_dirent *entry;
	int err;

	struct qstr name1 = { .name = (unsigned char *)"hello", .len = 5, };
	struct qstr name2 = { .name = (unsigned char *)"world", .len = 5, };
	struct inode *inode1 = rapid_new_inode(sb, NULL, S_IFREG);
	tux_inode(inode1)->inum = 0x666;
	struct inode *inode2 = rapid_new_inode(sb, NULL, S_IFLNK);
	tux_inode(inode2)->inum = 0x777;

	change_begin_atomic(sb);

	test_assert(tux_dir_is_empty(dir) == 0);

	err = tux_create_dirent(dir, &name1, inode1);
	test_assert(!err);
	err = tux_create_dirent(dir, &name2, inode2);
	test_assert(!err);

	entry = tux_find_dirent(dir, &name1, &buffer);
	test_assert(!IS_ERR(entry));
	test_assert(be64_to_cpu(entry->inum) == tux_inode(inode1)->inum);
	test_assert(be16_to_cpu(entry->rec_len) >= name1.len + 2);
	test_assert(entry->name_len == name1.len);
	test_assert(entry->type == TUX_REG);

	err = tux_delete_dirent(dir, buffer, entry);
	test_assert(!err);
	entry = tux_find_dirent(dir, &name1, &buffer);
	test_assert(IS_ERR(entry));

	entry = tux_find_dirent(dir, &name2, &buffer);
	test_assert(!IS_ERR(entry));
	test_assert(be64_to_cpu(entry->inum) == tux_inode(inode2)->inum);
	test_assert(be16_to_cpu(entry->rec_len) >= name2.len + 2);
	test_assert(entry->name_len == name2.len);
	test_assert(entry->type == TUX_LNK);
	blockput(buffer);

	test_assert(tux_dir_is_empty(dir) == -ENOTEMPTY);

	change_end_atomic(sb);

	rapid_free_inode(inode1);
	rapid_free_inode(inode2);
	clean_main(sb, dir);
}

static int filldir(struct dir_context *ctx, const char *name, int namelen,
		   loff_t offset, u64 inum, unsigned type)
{
	static int pos;

	char orig[100];
	sprintf(orig, "file%i", pos);
	trace_on("%s: name %*s, len %d, inum %Ld offset %Ld, type %x",
		 orig, namelen, name, namelen, inum, (s64)offset, type);

	if ((namelen == 1 && !memcmp(name, ".", 1)) ||
	    (namelen == 2 && !memcmp(name, "..", 2)))
		return 0;

	test_assert(memcmp(orig, name, strlen(orig)) == 0);
	test_assert(inum == pos+99);
	test_assert(type == DT_REG);

	pos++;

	return 0;
}

/* Test readdir */
static void test02(struct sb *sb, struct inode *dir)
{
	struct file *file = &(struct file)FILE_INIT(dir, 0);
	int err;

	change_begin_atomic(sb);

	int i = 0, changed = 0;
	loff_t size = 0;
	while (changed < 2) {
		struct inode *inode1 = rapid_new_inode(sb, NULL, S_IFREG);
		tux_inode(inode1)->inum = i + 99;

		char name[100];
		sprintf(name, "file%i", i);

		struct qstr qstr = {
			.name = (unsigned char *)name,
			.len = strlen(name),
		};
		err = tux_create_dirent(dir, &qstr, inode1);
		test_assert(!err);

		rapid_free_inode(inode1);

		if (size != dir->i_size) {
			size = dir->i_size;
			changed++;
		}

		i++;
	}

	struct dir_context ctx = {
		.actor	= filldir,
		.pos	= 0,
	};
	err = tux_readdir(file, &ctx);
	test_assert(!err);

	/* Test invalid offset (tux_readdir()'s revalidate) */
	ctx.pos -= TUX_DIR_ALIGN; /* set invalid offset */
	file->f_pos = ctx.pos;
	file->f_version = 0;
	err = tux_readdir(file, &ctx);
	test_assert(!err);
	test_assert(ctx.pos >= file->f_pos);

	change_end_atomic(sb);

	clean_main(sb, dir);
}

int main(int argc, char *argv[])
{
	test_init(argc, argv);

	struct dev *dev = &(struct dev){ .bits = 8 };

	int err = tux3_init_mem(1 << 20, 2);
	assert(!err);

	struct sb *sb = rapid_sb(dev);
	sb->super = INIT_DISKSB(dev->bits, 150);
	assert(!setup_sb(sb, &sb->super));
	assert(!set_blocksize(sb->blocksize));

	struct inode *dir = rapid_new_inode(sb, NULL, S_IFDIR);

	if (test_start("test01"))
		test01(sb, dir);
	test_end();

	if (test_start("test02"))
		test02(sb, dir);
	test_end();

	clean_main(sb, dir);
	return test_failures();
}
