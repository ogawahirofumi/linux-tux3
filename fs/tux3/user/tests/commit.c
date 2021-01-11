/*
 * Commit log and replay
 *
 * Original copyright (c) 2008 Daniel Phillips <phillips@phunq.net>
 * Licensed under the GPL version 3
 *
 * By contributing changes to this file you grant the original copyright holder
 * the right to distribute those changes under any license.
 */

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "tux3user.h"
#include "diskio.h"
#include "test.h"

#define trace trace_on

#include "tux3_fsck.c"

/* Make snapshot of volume */
static int snapshot_fd;

static void snapshot_volume(struct sb *sb)
{
	char templete[] = "test-XXXXXX";
	char buf[4096];
	int fd;

	fd = mkstemp(templete);
	assert(fd >= 0);
	unlink(templete);

	loff_t offset = 0;
	while (1) {
		ssize_t ret, ret2;

		ret = pread(sb_dev(sb)->fd, buf, sizeof(buf), offset);
		assert(ret >= 0);
		if (!ret)
			break;

		ret2 = pwrite(fd, buf, ret, offset);
		assert(ret == ret2);

		offset += ret;
	}

	snapshot_fd = fd;
}

static void restore_volume(struct sb *sb)
{
	char buf[4096];

	assert(snapshot_fd >= 0);

	loff_t offset = 0;
	while (1) {
		ssize_t ret, ret2;

		ret = pread(snapshot_fd, buf, sizeof(buf), offset);
		assert(ret >= 0);
		if (!ret)
			break;

		ret2 = pwrite(sb_dev(sb)->fd, buf, ret, offset);
		assert(ret == ret2);

		offset += ret;
	}
}

static void clean_snapshot(void)
{
	close(snapshot_fd);
	snapshot_fd = 0;
}

static block_t check_block;

static int check_defree_block(u64 val, void *data)
{
	block_t block = val & ((1ULL << 48) - 1);
	int count = val >> 48;
	if (block <= check_block && check_block < block + count)
		return -1;
	return 0;
}

/* Check if buffer is not freed blocks */
static int buffer_is_allocated(struct sb *sb, struct buffer_head *buf)
{
	check_block = bufindex(buf);
	if (stash_walk(&sb->defree.stash, check_defree_block, NULL) < 0)
		return 0; /* buffer is defree block */
	if (stash_walk(&sb->deunify.stash, check_defree_block, NULL) < 0)
		return 0; /* buffer is deunify block */
	/* Set fake backend mark to modify backend objects. */
	tux3_start_backend(sb);
	struct block_segment seg;
	unsigned blocks = 1;
	int segs = 0;
	int err = balloc_find_range(sb, &seg, 1, &segs, bufindex(buf), 1,
				    &blocks);
	test_assert(!err);
	tux3_end_backend();

	return blocks;	/* if blocks == 0, that block is free */
}

static void check_dirty_list(struct sb *sb, struct list_head *head)
{
	struct buffer_head *buf, *n;
	list_for_each_entry_safe(buf, n, head, link)
		test_assert(buffer_is_allocated(sb, buf));
}

/*
 * Check if freed blocks is not dirty
 * FIXME: this should move into put_super()?
 */
static void check_dirty(struct sb *sb)
{
	/* volmap only, because data buffers doesn't have block address yet */
	check_dirty_list(sb, tux3_dirty_buffers(sb->volmap, TUX3_INIT_DELTA));
	check_dirty_list(sb, &sb->unify_buffers);
}

struct open_result {
	char name[PATH_MAX];
	unsigned namelen;
	int err;
	inum_t inum;
	inum_t parent_inum;
};

static void fsck(struct sb *sb)
{
	test_assert(fsck_main(sb) == 0);
	put_super(sb);
}

static struct replay *check_replay(struct sb *sb)
{
	/* Replay, and read file back */
	struct replay *rp = __load_fs(sb);
	assert(!IS_ERR(rp));
	return rp;
}

static void reload_sb(struct sb *sb, int apply)
{
	test_assert(load_fs(sb, apply) == 0);
}

static void check_files(struct sb *sb, struct open_result *results, int nr)
{
	reload_sb(sb, 0);

	for (int i = 0; i < nr; i++) {
		struct open_result *r = &results[i];
		struct inode *inode, *dir = sb->rootdir;

		if (r->parent_inum)
			dir = tux3_iget(sb, r->parent_inum);
		else
			ihold(dir);
		test_assert(!IS_ERR(dir));

		inode = tuxopen(dir, r->name, r->namelen);
		if (IS_ERR(inode)) {
			test_assert(PTR_ERR(inode) == r->err);
			goto next;
		}

		struct tux3_inode *tuxnode = tux_inode(inode);
		test_assert(tuxnode->inum == r->inum);
		test_assert(tuxnode->parent_inum == r->parent_inum);
		iput(inode);
next:
		iput(dir);
	}
}

/* cleanup of sb */
static void clean_sb(struct sb *sb)
{
	/* Check if it didn't make strange dirty buffer */
	check_dirty(sb);
	put_super(sb);
}

static void clean_main(struct sb *sb)
{
	clean_sb(sb);
	tux3_exit_mem();
}

/* cleanup of main() after fsck() */
static void clean_main_and_fsck(struct sb *sb)
{
	clean_sb(sb);
	fsck(sb);
	tux3_exit_mem();
}

/* Generate all type of logs, and replay. */
static void test01(void *_arg)
{
	struct sb *sb = _arg;

	test_assert(mkfs_tux3(sb) == 0);

#define NUM_FILES	100
#define NUM_FAIL	5
	static struct open_result results[NUM_FILES + NUM_FAIL];

	struct tux_iattr iattr = { .mode = S_IFREG | S_IRWXU };
	struct inode *inode;

	/*
	 * This should make at least:
	 * LOG_UNIFY, LOG_BNODE_REDIRECT, LOG_FREEBLOCKS, LOG_BFREE_RELOG,
	 * LOG_BFREE_ON_UNIFY
	 */
	test_assert(force_unify(sb) == 0);
	test_assert(force_unify(sb) == 0);

	/*
	 * This should make at least:
	 * LOG_DELTA, LOG_BALLOC, LOG_BNODE_ROOT, LOG_BNODE_SPLIT,
	 * LOG_BNODE_ADD, LOG_BNODE_UPDATE, LOG_LEAF_REDIRECT, LOG_BFREE
	 */
	for (int i = 0; i < NUM_FILES; i++) {
		struct open_result *r = &results[i];

		r->namelen = snprintf(r->name, sizeof(r->name), "file%03d", i);
		inode = tuxcreate(sb->rootdir, r->name, r->namelen, &iattr);
		test_assert(!IS_ERR(inode));
		r->err = 0;
		r->inum = tux_inode(inode)->inum;

		/*
		 * This should make at least:
		 * LOG_BNODE_DEL, LOG_BNODE_ADJUST
		 *
		 * FIXME: to generate LOG_BNODE_MERGE, this should use
		 * punch hole, instead of truncate(). Then, read back
		 * to check punch hole working.
		 */
		if (i == NUM_FILES - 1) {
			struct file *file = &(struct file)FILE_INIT(inode, 0);
			char data[1024] = {};
			for (int j = 0; j < 1024; j++) {
				int size = tuxwrite(file, data, sizeof(data));
				test_assert(size == sizeof(data));
				/* commit to generates many extents */
				test_assert(force_delta(sb) == 0);
			}
			test_assert(tuxtruncate(inode, 0) == 0);
		}
		iput(inode);

		if ((i % 10) == 0)
			test_assert(force_delta(sb) == 0);
	}
	for (int i = NUM_FILES; i < NUM_FILES + NUM_FAIL; i++) {
		struct open_result *r = &results[i];

		snprintf(r->name, sizeof(r->name), "file%03d", i);
		r->err = -ENOENT;
	}

	check_dirty(sb);

	snapshot_volume(sb);

	if (test_start("test01.1")) {
		test_assert(force_delta(sb) == 0);
		clean_sb(sb);

		fsck(sb);

		check_files(sb, results, NUM_FILES + NUM_FAIL);
		clean_main(sb);
	}
	test_end();

	restore_volume(sb);

	if (test_start("test01.2")) {
		test_assert(force_unify(sb) == 0);
		clean_sb(sb);

		fsck(sb);

		check_files(sb, results, NUM_FILES + NUM_FAIL);
		clean_main(sb);
	}
	test_end();

	restore_volume(sb);
	test_assert(force_delta(sb) == 0);

	clean_snapshot();
	clean_main(sb);
}
TEST_DEFINE(TEST_UNIT, "test01", test01);

/* Test to unlink file before flushing */
/*  FIXME: check if I/O is nothing for inode */
static void test02(void *_arg)
{
	struct sb *sb = _arg;

	test_assert(mkfs_tux3(sb) == 0);
	test_assert(force_unify(sb) == 0);

	static struct open_result r;

	struct tux_iattr iattr = { .mode = S_IFREG | S_IRWXU };

	r.namelen = snprintf(r.name, sizeof(r.name), "file%03d", 1);
	r.err = -ENOENT;

	/* Create inode and write data without flush */
	struct inode *inode;
	inode = tuxcreate(sb->rootdir, r.name, r.namelen, &iattr);
	test_assert(!IS_ERR(inode));

	struct file *file = &(struct file)FILE_INIT(inode, 0);
	char buf[1024] = {};
	for (int i = 0; i < 1024; i++) {
		int size = tuxwrite(file, buf, sizeof(buf));
		test_assert(size == sizeof(buf));
	}
	iput(inode);
	/* unlink created inode */
	test_assert(tuxunlink(sb->rootdir, r.name, r.namelen) == 0);
	check_dirty(sb);

	/* Flush */
	test_assert(force_delta(sb) == 0);
	clean_sb(sb);

	fsck(sb);

	check_files(sb, &r, 1);
	clean_main(sb);
}
TEST_DEFINE(TEST_UNIT, "test02", test02);

/* Test to unlink file after flushing */
static void test03(void *_arg)
{
	struct sb *sb = _arg;

	test_assert(mkfs_tux3(sb) == 0);
	test_assert(force_unify(sb) == 0);

	static struct open_result r;

	struct tux_iattr iattr = { .mode = S_IFREG | S_IRWXU };

	r.namelen = snprintf(r.name, sizeof(r.name), "file%03d", 1);
	r.err = -ENOENT;

	/* Create inode and write data without flush */
	struct inode *inode;
	inode = tuxcreate(sb->rootdir, r.name, r.namelen, &iattr);
	test_assert(!IS_ERR(inode));

	struct file *file = &(struct file)FILE_INIT(inode, 0);
	char buf[1024] = {};
	for (int i = 0; i < 1024; i++) {
		int size = tuxwrite(file, buf, sizeof(buf));
		test_assert(size == sizeof(buf));
	}
	iput(inode);
	test_assert(force_delta(sb) == 0);

	snapshot_volume(sb);

	if (test_start("test03.1")) {
		/* unlink created inode */
		test_assert(tuxunlink(sb->rootdir, r.name, r.namelen) == 0);
		check_dirty(sb);

		/* Flush */
		test_assert(force_delta(sb) == 0);
		clean_sb(sb);

		fsck(sb);

		check_files(sb, &r, 1);
		clean_main(sb);
	}
	test_end();

	restore_volume(sb);

	if (test_start("test03.2")) {
		test_assert(force_unify(sb) == 0);

		/* unlink created inode */
		test_assert(tuxunlink(sb->rootdir, r.name, r.namelen) == 0);
		check_dirty(sb);

		/* Flush */
		test_assert(force_unify(sb) == 0);
		clean_sb(sb);

		fsck(sb);

		check_files(sb, &r, 1);
		clean_main(sb);
	}
	test_end();

	clean_snapshot();
	clean_main(sb);
}
TEST_DEFINE(TEST_UNIT, "test03", test03);

/* Create/write/unlink inode without flush */
static struct inode *make_orphan_inode(struct sb *sb, const char *name)
{
	static struct tux_iattr iattr = { .mode = S_IFREG | S_IRWXU };
	static char data[1024] = {};

	struct inode *inode;
	struct file *file;
	int err, size;

	inode = tuxcreate(sb->rootdir, name, strlen(name), &iattr);
	test_assert(!IS_ERR(inode));

	file = &(struct file)FILE_INIT(inode, 0);
	size = tuxwrite(file, data, sizeof(data));
	test_assert(size == sizeof(data));

	err = tuxunlink(sb->rootdir, name, strlen(name));
	assert(!err);

	return inode;
}

struct orphan_data {
	inum_t inum;
	int err;
};

static void check_orphan_inum(struct replay *rp, struct orphan_data *data,
			      int nr_data)
{
	struct sb *sb = rp->sb;

	for (int i = 0; i < nr_data; i++) {
		struct tux3_inode *tuxnode;
		struct list_head *head;
		int err = -ENOENT;
		head = &sb->orphan.add_head;
		list_for_each_entry(tuxnode, head, orphan_list) {
			if (data[i].inum == tuxnode->inum) {
				err = 0;
				break;
			}
		}
		head = &rp->orphan_in_otree;
		list_for_each_entry(tuxnode, head, orphan_list) {
			if (data[i].inum == tuxnode->inum) {
				err = 0;
				break;
			}
		}
		test_assert(data[i].err == err);
	}
}

/* Test for orphan inodes */
static void test04(void *_arg)
{
	struct sb *sb = _arg;

	test_assert(mkfs_tux3(sb) == 0);
	test_assert(force_unify(sb) == 0);

#define NR_ORPHAN	5
	struct orphan_data *data = test_alloc_shm(sizeof(*data) * NR_ORPHAN);

	/* Create on disk image to test lived orphan */
	pid_t pid = fork();
	assert(pid >= 0);
	if (pid == 0) {
		struct inode *inodes[NR_ORPHAN];
		LIST_HEAD(orphans);
		char name[] = "filename";

		/*
		 * inodes[0] is into sb->otree as orphan.
		 * inodes[1] is into, then delete from sb->otree
		 * inodes[2] is into sb->otree, and LOG_ORPHAN_DEL
		 * inodes[3] make LOG_ORPHAN_ADD
		 * inodes[4] make LOG_ORPHAN_ADD, and LOG_ORPHAN_DEL
		 */
		for (int i = 0; i < NR_ORPHAN; i++) {
			struct tux3_inode *tuxnode;

			inodes[i] = make_orphan_inode(sb, name);
			test_assert(!IS_ERR(inodes[i]));
			tuxnode = tux_inode(inodes[i]);

			data[i].inum = tuxnode->inum;

			switch (i) {
			case 0:
				data[i].err = 0;
				/* Add into sb->otree */
				test_assert(force_unify(sb) == 0);
				list_move(&tuxnode->orphan_list, &orphans);
				test_assert(sb->orphan.count == 1);
				test_assert(sb->orphan.count_del == 0);
				break;
			case 1:
			case 2:
				data[i].err = -ENOENT;
				/* Add into sb->otree */
				test_assert(force_unify(sb) == 0);
				iput(inodes[i]);
				if (i == 1) {
					test_assert(sb->orphan.count == 2);
					test_assert(sb->orphan.count_del == 0);

					/* Delete from sb->otree */
					test_assert(force_unify(sb) == 0);
					test_assert(sb->orphan.count == 1);
					test_assert(sb->orphan.count_del == 0);
				} else {
					/* Schedule delete from sb->otree */
					test_assert(force_delta(sb) == 0);
					test_assert(sb->orphan.count == 1);
					test_assert(sb->orphan.count_del == 1);
				}
				break;
			case 3:
				data[i].err = 0;
				/* Add LOG_ORPHAN_ADD */
				test_assert(force_delta(sb) == 0);
				list_move(&tuxnode->orphan_list, &orphans);
				test_assert(sb->orphan.count == 2);
				test_assert(sb->orphan.count_del == 1);
				break;
			case 4:
				/* Add LOG_ORPHAN_ADD and LOG_ORPHAN_DEL */
				data[i].err = -ENOENT;
				test_assert(force_delta(sb) == 0);
				test_assert(sb->orphan.count == 3);
				test_assert(sb->orphan.count_del == 1);

				iput(inodes[i]);

				test_assert(force_delta(sb) == 0);
				test_assert(sb->orphan.count == 2);
				test_assert(sb->orphan.count_del == 1);
				break;
			}
		}

		/* Hack: clean inodes without destroy */
		replay_iput_orphan_inodes(sb, &orphans, 0);

		clean_main(sb);
		/* Simulate crash */
		exit(1);
	}
	waitpid(pid, NULL, 0);
	clean_sb(sb);

	/* Check orphan btree and orphan logs */
	if (test_start("test04.1")) {
		/* Replay */
		struct replay *rp = check_replay(sb);

		test_assert(sb->orphan.count == 2);
		test_assert(sb->orphan.count_del == 1);

		/* Check orphan inodes */
		check_orphan_inum(rp, data, NR_ORPHAN);

		int err = replay_stage3(rp, 0);
		test_assert(!err);
		clean_sb(sb);

		fsck(sb);

		tux3_exit_mem();
	}
	test_end();

	/* Destroy orphans indoes and add orphan del log */
	if (test_start("test04.2")) {
		/* Replay */
		struct replay *rp = check_replay(sb);

		test_assert(sb->orphan.count == 2);
		test_assert(sb->orphan.count_del == 1);

		/* Destroy orphan inodes */
		int err = replay_stage3(rp, 1);
		test_assert(!err);

		/* Just add defer orphan deletion request */
		test_assert(force_delta(sb) == 0);
		test_assert(sb->orphan.count == 0);
		test_assert(sb->orphan.count_del == 2);

		clean_sb(sb);

		fsck(sb);

		tux3_exit_mem();
	}
	test_end();

	/* test04.2 destroyed orphans */
	for (int i = 0; i < NR_ORPHAN; i++)
		data[i].err = -ENOENT;

	/* Apply orphan del logs */
	if (test_start("test04.3")) {
		/* Replay */
		struct replay *rp = check_replay(sb);

		test_assert(sb->orphan.count == 0);
		test_assert(sb->orphan.count_del == 2);

		/* Check orphan inodes */
		check_orphan_inum(rp, data, NR_ORPHAN);

		int err = replay_stage3(rp, 1);
		test_assert(!err);

		/* Remove orphan from sb->otree */
		test_assert(force_unify(sb) == 0);
		test_assert(sb->orphan.count == 0);
		test_assert(sb->orphan.count_del == 0);

		clean_sb(sb);

		fsck(sb);

		tux3_exit_mem();
	}
	test_end();

	/* Check result */
	if (test_start("test04.4")) {
		/* Replay */
		struct replay *rp = check_replay(sb);

		test_assert(sb->orphan.count == 0);
		test_assert(sb->orphan.count_del == 0);

		/* Check orphan inodes */
		check_orphan_inum(rp, data, NR_ORPHAN);

		int err = replay_stage3(rp, 1);
		test_assert(!err);
		clean_sb(sb);

		fsck(sb);

		tux3_exit_mem();
	}
	test_end();

	tux3_exit_mem();
	test_free_shm(data, sizeof(*data) * NR_ORPHAN);
}
TEST_DEFINE(TEST_UNIT, "test04", test04);

/* Test for mkdir/rmdir */
static void test05(void *_arg)
{
	struct sb *sb = _arg;

	test_assert(mkfs_tux3(sb) == 0);
	test_assert(force_unify(sb) == 0);

	static struct open_result r;

	struct tux_iattr iattr = { .mode = S_IFDIR | 0755 };

	r.namelen = snprintf(r.name, sizeof(r.name), "dir%03d", 1);
	r.err = -ENOENT;

	/* Create dir and add some dirent without flush */
	struct inode *dir;
	dir = tuxcreate(sb->rootdir, r.name, r.namelen, &iattr);
	test_assert(!IS_ERR(dir));

	/* mkdir and rmdir subdir, this adds at least 1 buffer to dir */
	struct inode *subdir;
	const char *subname = "subdir";
	subdir = tuxcreate(dir, subname, strlen(subname), &iattr);
	test_assert(!IS_ERR(subdir));
	iput(subdir);
	test_assert(tuxrmdir(dir, subname, strlen(subname)) == 0);

	iput(dir);

	snapshot_volume(sb);

	/* rmdir after flush */
	if (test_start("test05.1")) {
		test_assert(force_delta(sb) == 0);

		/* rmdir created dir */
		test_assert(tuxrmdir(sb->rootdir, r.name, r.namelen) == 0);
		check_dirty(sb);

		/* Flush */
		test_assert(force_delta(sb) == 0);
		clean_sb(sb);

		fsck(sb);

		check_files(sb, &r, 1);
		clean_main(sb);
	}
	test_end();

	restore_volume(sb);

	/* rmdir before flush */
	if (test_start("test05.2")) {
		/* rmdir created dir */
		test_assert(tuxrmdir(sb->rootdir, r.name, r.namelen) == 0);
		check_dirty(sb);

		/* Flush */
		test_assert(force_delta(sb) == 0);
		clean_sb(sb);

		fsck(sb);

		check_files(sb, &r, 1);
		clean_main(sb);
	}
	test_end();

	restore_volume(sb);
	test_assert(force_delta(sb) == 0);

	clean_snapshot();
	clean_main(sb);
}
TEST_DEFINE(TEST_UNIT, "test05", test05);

static void check_parent_inum(struct inode *dir, const char *name, int len)
{
	struct inode *inode = tuxopen(dir, name, len);
	test_assert(!IS_ERR(inode));
	test_assert(tux_inode(inode)->parent_inum == tux_inode(dir)->inum);
	iput(inode);
}

/* Test for rename */
static void test06(void *_arg)
{
	struct sb *sb = _arg;

	test_assert(mkfs_tux3(sb) == 0);
	test_assert(force_unify(sb) == 0);

	enum { F, D, C, C2, B, A, B2, O, };
	static struct open_result r[] = {
		[F] = {
			.name		= "file",
			.namelen	= 4,
			.err		= 0,
		},
		[D] = {
			.name		= "dir",
			.namelen	= 3,
			.err		= 0,
		},
		[C] = {
			.name		= "child",
			.namelen	= 5,
			.err		= 0,
		},
		[C2] = {
			.name		= "child2",
			.namelen	= 6,
			.err		= 0,
		},
		[B] = {
			.name		= "before",
			.namelen	= 6,
			.err		= -ENOENT,
		},
		[A] = {
			.name		= "after",
			.namelen	= 5,
			.err		= 0,
		},
		[B2] = {
			.name		= "before2",
			.namelen	= 7,
			.err		= -ENOENT,
		},
		[O] = {
			.name		= "overwrite",
			.namelen	= 9,
			.err		= 0,
		},
	};

	struct tux_iattr iattr = { .mode = S_IFDIR | 0755 };

	/* Test create("file"). */
	struct inode *inode;
	iattr.mode = S_IFREG | 0755;
	inode = tuxcreate(sb->rootdir, r[F].name, r[F].namelen, &iattr);
	test_assert(!IS_ERR(inode));
	r[F].inum = tux_inode(inode)->inum;
	iput(inode);

	/* Test create("dir"). */
	struct inode *dir;
	iattr.mode = S_IFDIR | 0755;
	dir = tuxcreate(sb->rootdir, r[D].name, r[D].namelen, &iattr);
	test_assert(!IS_ERR(dir));
	r[D].inum = tux_inode(dir)->inum;
	r[D].parent_inum = tux_inode(sb->rootdir)->inum;
	check_parent_inum(sb->rootdir, r[D].name, r[D].namelen);
	/* iput(dir); */

	/* Test create("child"). */
	struct inode *child;
	iattr.mode = S_IFDIR | 0755;
	child = tuxcreate(sb->rootdir, r[C].name, r[C].namelen, &iattr);
	test_assert(!IS_ERR(child));
	r[C].inum = tux_inode(child)->inum;
	r[C].parent_inum = tux_inode(sb->rootdir)->inum;
	check_parent_inum(sb->rootdir, r[C].name, r[C].namelen);
	iput(child);

	/* Test create("child2"). */
	struct inode *child2;
	iattr.mode = S_IFDIR | 0755;
	child2 = tuxcreate(sb->rootdir, r[C2].name, r[C2].namelen, &iattr);
	test_assert(!IS_ERR(child2));
	r[C2].inum = tux_inode(child2)->inum;
	r[C2].parent_inum = tux_inode(dir)->inum;
	check_parent_inum(sb->rootdir, r[C2].name, r[C2].namelen);
	iput(child2);

	/* Test mkdir("before"), then rename("before", "after"). */
	struct inode *subdir;
	subdir = tuxcreate(sb->rootdir, r[B].name, r[B].namelen, &iattr);
	test_assert(!IS_ERR(subdir));
	r[B].inum = tux_inode(subdir)->inum;
	r[B].parent_inum = tux_inode(sb->rootdir)->inum;
	r[A].parent_inum = tux_inode(sb->rootdir)->inum;
	check_parent_inum(sb->rootdir, r[B].name, r[B].namelen);
	iput(subdir);

	/*
	 * Test mkdir("before2") and mkdir("overwrite"), then
	 * rename("before2", "overwrite").
	 */
	subdir = tuxcreate(sb->rootdir, r[B2].name, r[B2].namelen, &iattr);
	test_assert(!IS_ERR(subdir));
	r[B2].inum = tux_inode(subdir)->inum;
	r[B2].parent_inum = tux_inode(sb->rootdir)->inum;
	check_parent_inum(sb->rootdir, r[B2].name, r[B2].namelen);
	iput(subdir);

	subdir = tuxcreate(sb->rootdir, r[O].name, r[O].namelen, &iattr);
	test_assert(!IS_ERR(subdir));
	r[O].inum = tux_inode(subdir)->inum;
	r[O].parent_inum = tux_inode(sb->rootdir)->inum;
	check_parent_inum(sb->rootdir, r[O].name, r[O].namelen);
	iput(subdir);
	/* Check inum is not same */
	test_assert(r[B2].inum != r[O].inum);

	snapshot_volume(sb);

	const char *tests[] = {
		"test06.1", /* Test rename after flush */
		"test06.2", /* Test rename before flush */
	};
	for (int i = 0; i < 2; i++) {
		if (test_start(tests[i])) {
			if (i == 0)
				test_assert(force_delta(sb) == 0);

			int err;
			/* Test rename("file", "dir") */
			err = tuxrename(sb->rootdir, r[F].name, r[F].namelen,
					sb->rootdir, r[D].name, r[D].namelen,
					0);
			test_assert(err == -EISDIR);

			/* Test rename("dir", "file") */
			err = tuxrename(sb->rootdir, r[D].name, r[D].namelen,
					sb->rootdir, r[F].name, r[F].namelen,
					0);
			test_assert(err == -ENOTDIR);

			/* Test rename("child", "dir/child") */
			unsigned int nlink = dir->i_nlink;
			err = tuxrename(sb->rootdir, r[C].name, r[C].namelen,
					dir, r[C].name, r[C].namelen,
					0);
			test_assert(err == 0);
			test_assert(dir->i_nlink == nlink + 1);
			check_parent_inum(dir, r[C].name, r[C].namelen);

			/* Test rename("dir/child", "child") */
			err = tuxrename(dir, r[C].name, r[C].namelen,
					sb->rootdir, r[C].name, r[C].namelen,
					0);
			test_assert(err == 0);
			test_assert(dir->i_nlink == nlink);
			check_parent_inum(sb->rootdir, r[C].name, r[C].namelen);

			/* Test rename("child2", "dir/child2") */
			nlink = dir->i_nlink;
			err = tuxrename(sb->rootdir, r[C2].name, r[C2].namelen,
					dir, r[C2].name, r[C2].namelen,
					0);
			test_assert(err == 0);
			test_assert(dir->i_nlink == nlink + 1);
			check_parent_inum(dir, r[C2].name, r[C2].namelen);
			iput(dir);

			/* Test rename("before", "after") */
			err = tuxrename(sb->rootdir, r[B].name, r[B].namelen,
					sb->rootdir, r[A].name, r[A].namelen,
					0);
			test_assert(!err);
			/* Update inum for rename test */
			r[A].inum = r[B].inum;
			check_parent_inum(sb->rootdir, r[A].name, r[A].namelen);

			/* Test rename("before2", "overwrite") */
			err = tuxrename(sb->rootdir, r[B2].name, r[B2].namelen,
					sb->rootdir, r[O].name, r[O].namelen,
					0);
			test_assert(!err);
			/* Update inum for rename test */
			r[O].inum = r[B2].inum;
			check_parent_inum(sb->rootdir, r[O].name, r[O].namelen);

			check_dirty(sb);

			/* Flush */
			test_assert(force_delta(sb) == 0);
			clean_sb(sb);

			fsck(sb);

			check_files(sb, r, ARRAY_SIZE(r));
			clean_main(sb);
		}
		test_end();

		restore_volume(sb);
	}

	iput(dir);
	test_assert(force_delta(sb) == 0);

	clean_snapshot();
	clean_main(sb);
}
TEST_DEFINE(TEST_UNIT, "test06", test06);

static void add_dirty_inode(struct sb *sb)
{
	/* Add dirty inode, but no actual flushing data */
	change_begin(sb, 0); /* ignore ENOSPC */
	__tux3_mark_inode_dirty(sb->rootdir, I_DIRTY_PAGES);
	change_end(sb);
}

/* Test for partial alloc to flush logblocks */
static void test07(void *_arg)
{
	struct sb *sb = _arg;

	test_assert(mkfs_tux3(sb) == 0);
	test_assert(force_unify(sb) == 0);

	/* Make dirty inode to workaround tux3_has_dirty_inodes() check */
	add_dirty_inode(sb);

	tux3_start_backend(sb);
	/* Make non contiguous blocks */
	for (block_t i = 0; i < sb->volblocks; i += 2) {
		struct block_segment seg;
		unsigned blocks = 1;
		int err, segs = 0;

		err = balloc_find_range(sb, &seg, 1, &segs, i, 1, &blocks);
		test_assert(!err);
		if (blocks == 0)
			test_assert(!balloc_use(sb, &seg, 1));
	}
	/* Make 3 logblocks, at least */
	while (sb->logpos.next <= 3)
		log_delta(sb);
	tux3_end_backend();

	/* Flush logblocks */
	test_assert(force_delta(sb) == 0);

	clean_main(sb);
}
TEST_DEFINE(TEST_UNIT, "test07", test07);

/* Test for replay of LOG_BNODE_FREE order */
static void test08(void *_arg)
{
	struct sb *sb = _arg;

	test_assert(mkfs_tux3(sb) == 0);

	struct tux_iattr iattr = { .mode = S_IFREG | S_IRWXU };
	struct inode *inode;

	/* Create files until itree has a bnode */
	int n = 1, i = 1;
	while (itree_btree(sb)->root.depth <= 1) {
		char tmp[] = "bbb";
		tmp[n] = i;
		inode = tuxcreate(sb->rootdir, tmp, strlen(tmp), &iattr);
		test_assert(inode);
		struct file *file = &(struct file)FILE_INIT(inode, 0);
		char buf[10] = {};
		test_assert(tuxwrite(file, buf, sizeof(buf)) == sizeof(buf));
		iput(inode);
		test_assert(force_delta(sb) == 0);

		i++;
		if (i == '\0' || i == '\\')
			i++;
		if (i == 0xff) {
			i = 1;
			n++;
		}
	}

	const char name[] = "a";
	inode = tuxcreate(sb->rootdir, name, strlen(name), &iattr);
	test_assert(inode);
	struct file *file = &(struct file)FILE_INIT(inode, 0);
	char buf[10] = {};
	test_assert(tuxwrite(file, buf, sizeof(buf)) == sizeof(buf));
	test_assert(force_delta(sb) == 0);

	/* "freed_block" should recorded as LOG_BNODE_FREE */
	struct btree *btree = &tux_inode(inode)->btree;
	block_t freed_block = btree->root.block;
	tuxunlink(sb->rootdir, name, strlen(name));
	iput(inode);
	test_assert(force_delta(sb) == 0);

	/* Make dirty inode to workaround tux3_has_dirty_inodes() check */
	add_dirty_inode(sb);

	/* Reuse "freed_block" for redirect target */
	tux3_start_backend(sb);
	struct btree *itree = itree_btree(sb);
	block_t oldblock = itree->root.block;
	log_bnode_redirect(sb, oldblock, freed_block);
	itree->root.block = freed_block;
	tux3_end_backend();

	/* Flush logblocks */
	test_assert(force_delta(sb) == 0);

	clean_main_and_fsck(sb);
}
TEST_DEFINE(TEST_UNIT, "test08", test08);

/* Test for cross boundary allocation on countmap group and fsck */
static void test09(void *_arg)
{
	struct sb *sb = _arg;

	test_assert(mkfs_tux3(sb) == 0);
	test_assert(force_unify(sb) == 0);

	unsigned align = 1 << sb->groupbits;
	block_t start = align * 3;	/* pick free blocks on boundary */

	/* Make dirty inode to workaround tux3_has_dirty_inodes() check */
	add_dirty_inode(sb);

	tux3_start_backend(sb);
	/* Allocate cross boundary blocks */
	struct block_segment seg[10];
	unsigned blocks = 10;
	int err, segs = 0;

	err = balloc_find_range(sb, seg, ARRAY_SIZE(seg), &segs,
				start - (blocks / 2), 100, &blocks);
	test_assert(!err);
	test_assert(segs == 1);	/* supporting cross boundary allocation? */
	test_assert(!blocks);
	test_assert(!balloc_use(sb, seg, segs));
	/* Setup log records for above blocks to pass fsck */
	log_balloc(sb, seg[0].block, seg[0].count);
	log_bfree_on_unify(sb, seg[0].block, seg[0].count);
	tux3_end_backend();

	/* Flush logblocks */
	test_assert(force_delta(sb) == 0);

	clean_main_and_fsck(sb);
}
TEST_DEFINE(TEST_UNIT, "test09", test09);

/* Test for mount options */
static void test10(void *_arg)
{
	struct sb *sb = _arg;

	test_assert(mkfs_tux3(sb) == 0);

	char options[512];
	char buf[512];
	ssize_t size_all, size;
	int err;

	/* Should have default options */
	test_assert(sb->mopt.flags == tux3_default_mopt.flags);

	/* Set invalid option */
	strcpy(options, "barrier,foo,nobarrier");
	err = setup_mount_options(sb, options);
	test_assert(err < 0);
	/* Clear barrier */
	strcpy(options, "nobarrier");
	err = setup_mount_options(sb, options);
	test_assert(!err);
	test_assert(!TUX3_TEST_MOPT(sb, BARRIER));
	/* Set barrier */
	strcpy(options, "barrier");
	err = setup_mount_options(sb, options);
	test_assert(!err);
	test_assert(TUX3_TEST_MOPT(sb, BARRIER));

	/* Show all options */
	size_all = get_mount_options(sb, buf, sizeof(buf), 1);
	test_assert(size_all > 0);
	test_assert(size_all == strlen(buf) + 1);
	/* Show options except default */
	size = get_mount_options(sb, buf, sizeof(buf), 0);
	test_assert(size_all >= size);
	test_assert(size == strlen(buf) + 1);
	/* Too small buffer */
	size = get_mount_options(sb, buf, 1, 1);
	test_assert(size < 0);

	clean_main_and_fsck(sb);
}
TEST_DEFINE(TEST_UNIT, "test10", test10);

/* Test for non regular file types */
static void test11(void *_arg)
{
	struct sb *sb = _arg;

	test_assert(mkfs_tux3(sb) == 0);

	struct tux_iattr iattr = {
		.uid = GLOBAL_ROOT_UID,
		.gid = GLOBAL_ROOT_GID,
	};
	const char name[] = "filename";
	const char target[] = "../foo/bar/test";
	static char buf[1 << 16];
	int err;

	/* Create too long symlink */
	memset(buf, 'a', sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	err = tuxsymlink(sb->rootdir, name, strlen(name), &iattr, buf);
	test_assert(err == -ENAMETOOLONG);

	/* Create empty symlink */
	err = tuxsymlink(sb->rootdir, name, strlen(name), &iattr, "");
	test_assert(err == -ENOENT);

	/* Create normal symlink */
	err = tuxsymlink(sb->rootdir, name, strlen(name), &iattr, target);
	test_assert(!err);

	/* Create same symlink again */
	err = tuxsymlink(sb->rootdir, name, strlen(name), &iattr, target);
	test_assert(err == -EEXIST);

	int len;
	/* Read symlink */
	len = tuxreadlink(sb->rootdir, name, strlen(name), buf, sizeof(buf));
	test_assert(len == strlen(target));
	buf[len] = '\0';
	test_assert(!strcmp(buf, target));

	/* Read symlink with too small buffer */
	buf[1] = '#';
	len = tuxreadlink(sb->rootdir, name, strlen(name), buf, 1);
	test_assert(len == 1);
	test_assert(buf[0] == target[0]);
	test_assert(buf[1] == '#');

	/* Read symlink with no exist name */
	const char not[] = "not";
	err = tuxreadlink(sb->rootdir, not, strlen(not), buf, sizeof(buf));
	test_assert(err == -ENOENT);

	/* Create hardlink */
	const char *a1 = name;
	char a2[] = "name_dst";
	err = tuxlink(sb->rootdir, a1, strlen(a1), a2, strlen(a2));
	test_assert(!err);

	/* Create exist destination */
	err = tuxlink(sb->rootdir, a1, strlen(a1), a2, strlen(a2));
	test_assert(err == -EEXIST);

	/* Create hardlink for dir */
	char d1[] = "dir";
	char d2[] = "dir_dst";
	iattr.mode = S_IFDIR | 0755;
	struct inode *inode = tuxcreate(sb->rootdir, d1, strlen(d1), &iattr);
	test_assert(!IS_ERR(inode));
	iput(inode);
	err = tuxlink(sb->rootdir, d1, strlen(d1), d2, strlen(d2));
	test_assert(err == -EPERM);

	/* Create no exist source */
	char n1[] = "no";
	char n2[] = "no_dst";
	err = tuxlink(sb->rootdir, n1, strlen(n1), n2, strlen(n2));
	test_assert(err == -ENOENT);

	/* Flush */
	test_assert(force_delta(sb) == 0);

	clean_main_and_fsck(sb);
}
TEST_DEFINE(TEST_UNIT, "test11", test11);

/* Test for reading saved inodes from disk  */
static void test12(void *_arg)
{
	struct sb *sb = _arg;

	test_assert(mkfs_tux3(sb) == 0);

#define NR_REG		10
#define NR_NON_REG	4
	struct tux_iattr iattr = { .mode = S_IFREG | S_IRWXU };
	struct entry {
		struct inode inode;
		char name[256];
		int len;
	} entries[NR_REG + NR_NON_REG];

	/* Create and remember inode */
	for (int i = 0; i < NR_REG; i++) {
		struct entry *e = &entries[i];
		struct inode *inode;

		e->len = snprintf(e->name, sizeof(e->name), "file%03d", i);
		inode = tuxcreate(sb->rootdir, e->name, e->len, &iattr);
		test_assert(!IS_ERR(inode));
		test_assert(inode->i_mode == iattr.mode);

		e->inode = *inode;

		iput(inode);
	}
	/* Create non regular files */
	struct tux_iattr rdevs[NR_NON_REG] = {
		{ .mode = S_IFIFO | 0644, },
		{ .mode = S_IFSOCK | 0644, },
		{ .mode = S_IFCHR | 0644, .rdev = MKDEV(10, 10), },
		{ .mode = S_IFBLK | 0644, .rdev = MKDEV(11, 11), },
	};
	for (int i = NR_REG; i < (NR_REG + NR_NON_REG); i++) {
		struct entry *e = &entries[i];
		struct inode *inode;
		struct tux_iattr *iattrp = &rdevs[i - NR_REG];

		e->len = snprintf(e->name, sizeof(e->name), "file%03d", i);
		inode = tuxmknod(sb->rootdir, e->name, e->len, iattrp);
		test_assert(!IS_ERR(inode));
		test_assert(inode->i_mode == iattrp->mode);
		test_assert(inode->i_rdev == iattrp->rdev);

		e->inode = *inode;

		iput(inode);
	}

	/* Flush */
	test_assert(force_unify(sb) == 0);
	clean_sb(sb);

	fsck(sb);

	reload_sb(sb, 1);

	/* Load and compare inode */
	for (int i = 0; i < ARRAY_SIZE(entries); i++) {
		struct entry *e = &entries[i];
		struct inode *inode;

		inode = tuxopen(sb->rootdir, e->name, e->len);
		test_assert(!IS_ERR(inode));

		test_assert(e->inode.i_mode == inode->i_mode);
		test_assert(uid_eq(e->inode.i_uid, inode->i_uid));
		test_assert(gid_eq(e->inode.i_gid, inode->i_gid));
		test_assert(e->inode.i_nlink == inode->i_nlink);
		test_assert(e->inode.i_rdev == inode->i_rdev);
		test_assert(e->inode.i_size == inode->i_size);
#if 0 /* FIXME: atime is not supported yet */
		test_assert(timespec64_equal(&e->inode.i_atime, &inode->i_atime));
#endif
		test_assert(timespec64_equal(&e->inode.i_mtime, &inode->i_mtime));
		test_assert(timespec64_equal(&e->inode.i_ctime, &inode->i_ctime));
		test_assert(inode_peek_iversion(&e->inode) == inode_peek_iversion(inode));

		iput(inode);
	}

	clean_main(sb);
}
TEST_DEFINE(TEST_UNIT, "test12", test12);

int main(int argc, char *argv[])
{
	int argi = test_init(argc, argv);

	if (argi >= argc)
		error_exit("usage: %s <volname>", argv[0]);

	int fd = open(argv[argi], O_CREAT|O_TRUNC|O_RDWR, S_IRUSR|S_IWUSR);
	assert(fd >= 0);
	u64 volsize = 1 << 24;
	int err = ftruncate(fd, volsize);
	assert(!err);

	err = tux3_init_mem(1 << 24, 2);
	assert(!err);

	struct dev *dev = &(struct dev){ .fd = fd, .bits = 8 };
	struct sb *sb = rapid_sb(dev);
	sb->super = INIT_DISKSB(dev->bits, volsize >> dev->bits);

	test_run(sb);

	tux3_exit_mem();
	return test_failures();
}
