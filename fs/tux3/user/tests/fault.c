/*
 * Error paths
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

/* Enable fault injection codes */
#define FAULT_INJECTION		1
#include "fault_inject.h"
#include "fault_inject.c"
#include "utility.c"

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
};

static void fsck(struct sb *sb)
{
	test_assert(fsck_main(sb) == 0);
	put_super(sb);
}

/* cleanup of main() after fsck() */
static void clean_main_and_fsck(struct sb *sb)
{
	check_dirty(sb);
	put_super(sb);
	fsck(sb);
	tux3_exit_mem();
}

#define TEST_CONFIG	(FAULT_ONE_SHOT | FAULT_PER_CALLPATH)

/* Generate all type of logs, and replay. */
static void test01(struct sb *sb)
{
	int nr, err;

	nr = 0;
	err = -1;
	while (err) {
		printf("---- __mkfs_tux3 %d ----\n", nr);

//		fault_enable("io:*", TEST_CONFIG, 100);
//		fault_enable("memory:*", TEST_CONFIG, 100);

		err = __mkfs_tux3(sb);

		struct fault_info *info = fault_last_inject();
		/* new_buffer() for readahead is non fatal error */
		if (info && strcmp(info->func, "new_buffer") != 0)
			test_assert(err);
		fault_clear_last();

		if (err) {
			fault_disable("*");
			put_super(sb);
		}

		nr++;
	}
	nr = 0;
	err = -1;
	while (err) {
		printf("---- sync_super %d ----\n", nr);

		err = sync_super(sb);

		nr++;
	}

	fault_disable("*");

	clean_main_and_fsck(sb);
}

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

	err = tux3_init_mem(volsize, 2);
	assert(!err);

	struct dev *dev = &(struct dev){ .fd = fd, .bits = 8 };
	struct sb *sb = rapid_sb(dev);
	sb->super = INIT_DISKSB(dev->bits, volsize >> dev->bits);

	if (test_start("test01"))
		test01(sb);
	test_end();

	tux3_exit_mem();
	return test_failures();
}
