#ifndef trace
#define trace trace_off
#endif

#include "../filemap.c"
#include "test.h"

struct test_arg {
	struct sb *sb;
	struct inode *inode;
};

#define FOO	"foo"

static void clean_main(struct sb *sb, struct inode *inode)
{
	iput(inode);
	put_super(sb);
	tux3_exit_mem();
}

static void add_maps(struct inode *inode, block_t index,
		     struct block_segment *seg, int nr_segs,
		     enum map_mode mode)
{
	unsigned delta = tux3_get_current_delta();

	for (int i = 0; i < nr_segs; i++) {
		struct block_segment *s = &seg[i];
		for (unsigned j = 0; j < s->count; j++) {
			block_t block = s->block + j;
			struct buffer_head *buf;

			/* Check overwritten seg has expected physical */
			buf = blockget(mapping(inode), index + j);
			if (buffer_dirty(buf)) {
				block_t old_block = *(block_t *)buf->data;
				if (mode == MAP_REDIRECT)
					test_assert(block != old_block);
				else
					test_assert(block == old_block);
			}

			buf = blockdirty(buf, delta);
			memset(buf->data, 0, tux_sb(inode->i_sb)->blocksize);
			*(block_t *)buf->data = block;
			mark_buffer_dirty_non(buf);
			blockput(buf);
		}
		index += s->count;
	}
}

/* Create segments, then save state to buffer */
static int d_filemap(struct inode *inode, block_t start, unsigned count,
		     struct block_segment *seg, unsigned seg_max,
		     enum map_mode mode)
{
	int segs;
	/* this should be called with "mode != MAP_READ" */
	assert(mode != MAP_READ);

	disable_vol_early_io = true;
	segs = filemap(inode, start, count, seg, seg_max, mode);
	disable_vol_early_io = false;
	tux_sb(inode->i_sb)->last_dleaf = NULL;
	if (segs > 0)
		add_maps(inode, start, seg, segs, mode);
	return segs;
}

/* Handle partial write */
static int d_filemaps(struct inode *inode, block_t start, unsigned count,
		      struct block_segment *seg, unsigned seg_max,
		      enum map_mode mode)
{
	int total = 0;

	while (count) {
		int segs = d_filemap(inode, start, count, seg, seg_max, mode);

		total += segs;
		while (segs-- > 0) {
			start += seg->count;
			count -= seg->count;
			seg++;
			seg_max--;
		}
	}

	return total;
}

static void check_maps(struct inode *inode, block_t index,
		       struct block_segment *seg, int nr_segs)
{
	for (int i = 0; i < nr_segs; i++) {
		struct block_segment *s = &seg[i];
		for (unsigned j = 0; j < s->count; j++) {
			struct buffer_head *buf;
			buf = peekblk(inode->map, index + j);
			if (s->state == BLOCK_SEG_HOLE)
				test_assert(buf == NULL);
			else {
				block_t blk = *(block_t *)buf->data;
				test_assert(blk == s->block + j);
				blockput(buf);
			}
		}
		index += s->count;
	}
}

/* Check returned segments are same state with buffer */
static int check_filemap(struct inode *inode, block_t start, unsigned count,
			 struct block_segment *seg, unsigned seg_max)
{
	int segs;
	segs = filemap(inode, start, count, seg, seg_max, MAP_READ);
	if (segs > 0)
		check_maps(inode, start, seg, segs);
	return segs;
}

struct test_data {
	block_t index;
	unsigned count;
	enum map_mode mode;
};

/* Test basic operations */
static void test01(void *_arg)
{
	struct test_arg *arg = _arg;
	struct sb *sb = arg->sb;
	struct inode *inode = arg->inode;

	/*
	 * FIXME: filemap() are not supporting to read segments on
	 * multiple leaves at once.
	 */
#define CAN_HANDLE_A_LEAF	1
#define NR			30

	/* Create by ascending order */
	if (test_start("test01.1")) {
		struct block_segment seg;
		int err, segs;

		/* Set fake backend mark to modify backend objects. */
		tux3_start_backend(sb);

		for (int i = 0, j = 0; i < NR; i++, j++) {
			segs = d_filemaps(inode, 2*i, 1, &seg, 1, MAP_REDIRECT);
			test_assert(segs == 1);
		}
#ifdef CAN_HANDLE_A_LEAF
		for (int i = 0; i < NR; i++) {
			segs = check_filemap(inode, 2*i, 1, &seg, 1);
			test_assert(segs == 1);
		}
#else
		segs = check_filemap(inode, 0, NR*2, seg, ARRAY_SIZE(seg));
		test_assert(segs == NR*2);
#endif

		/* btree_chop and dleaf_chop test */
		int index = (NR + 1)*2;
		while (index--) {
			err = btree_chop(&tux_inode(inode)->btree, index,
					 TUXKEY_LIMIT);
			test_assert(!err);
#ifdef CAN_HANDLE_A_LEAF
			for (int i = 0; i < NR; i++) {
				if (index <= i*2)
					break;
				segs = check_filemap(inode, 2*i, 1, &seg, 1);
				test_assert(segs == 1);
			}
#else
			segs = check_filemap(inode, 0, NR*2, seg,
					     ARRAY_SIZE(seg));
			test_assert(segs == i*2);
#endif
		}

		/* Check if truncated all */
		segs = filemap(inode, 0, INT_MAX, &seg, 1, MAP_READ);
		test_assert(segs == 1);
		test_assert(seg.count == INT_MAX);
		test_assert(seg.state == BLOCK_SEG_HOLE);

		tux3_end_backend();

		test_assert(force_delta(sb) == 0);
		clean_main(sb, inode);
	}
	test_end();

	/* Create by descending order */
	if (test_start("test01.2")) {
		struct block_segment seg;
		int err, segs;

		/* Set fake backend mark to modify backend objects. */
		tux3_start_backend(sb);

		for (int i = NR; i >= 0; i--) {
			segs = d_filemaps(inode, 2*i, 1, &seg, 1, MAP_REDIRECT);
			test_assert(segs == 1);
		}
#ifdef CAN_HANDLE_A_LEAF
		for (int i = NR; i >= 0; i--) {
			segs = check_filemap(inode, 2*i, 1, &seg, 1);
			test_assert(segs == 1);
		}
#else
		segs = check_filemap(inode, 0, NR*2, seg, ARRAY_SIZE(seg));
		test_assert(segs == i*2);
#endif

		err = btree_chop(&tux_inode(inode)->btree, 0, TUXKEY_LIMIT);
		test_assert(!err);

		/* Check if truncated all */
		segs = filemap(inode, 0, INT_MAX, &seg, 1, MAP_READ);
		test_assert(segs == 1);
		test_assert(seg.count == INT_MAX);
		test_assert(seg.state == BLOCK_SEG_HOLE);

		tux3_end_backend();

		test_assert(force_delta(sb) == 0);
		clean_main(sb, inode);
	}
	test_end();

	test_assert(force_delta(sb) == 0);
	clean_main(sb, inode);
}
TEST_DEFINE(TEST_UNIT, "test01", test01);

/* Test redirect mode (create == 2) */
static void test02(void *_arg)
{
	struct test_arg *arg = _arg;
	struct sb *sb = arg->sb;
	struct inode *inode = arg->inode;

	struct block_segment seg[32];

	struct test_data data[] = {
		{ .index = 5,  .count = 64, .mode = MAP_WRITE, },
		{ .index = 10, .count = 20, .mode = MAP_REDIRECT, },
		{ .index = 80, .count = 10, .mode = MAP_REDIRECT, },
	};

	/* Set fake backend mark to modify backend objects. */
	tux3_start_backend(sb);

	int total_segs = 0;
	for (int i = 0; i < ARRAY_SIZE(data); i++) {
		int segs1, segs2;

		segs1 = d_filemaps(inode, data[i].index, data[i].count,
				   seg, ARRAY_SIZE(seg), data[i].mode);
		test_assert(segs1 > 0);
		total_segs += segs1;

		segs2 = check_filemap(inode, data[i].index, data[i].count,
				      seg, ARRAY_SIZE(seg));
		test_assert(segs1 == segs2);
	}

	/* Check whole rage from 0 */
	int segs = check_filemap(inode, 0, 200, seg, ARRAY_SIZE(seg));
	test_assert(segs >= total_segs);

	tux3_end_backend();

	/* Clear dirty page to prevent to call filemap again */
	change_begin_atomic(sb);
	truncate_inode_pages(mapping(inode), 0);
	change_end_atomic(sb);

	test_assert(force_delta(sb) == 0);
	clean_main(sb, inode);
}
TEST_DEFINE(TEST_UNIT, "test02", test02);

/* Test overwrite seg entirely inside existing */
static void test03(void *_arg)
{
	struct test_arg *arg = _arg;
	struct sb *sb = arg->sb;
	struct inode *inode = arg->inode;

	struct block_segment seg1[32], seg2[32];
	int segs1, segs2;

	/* Set fake backend mark to modify backend objects. */
	tux3_start_backend(sb);

	/* Create range */
	segs1 = d_filemaps(inode, 2, 5, seg1, ARRAY_SIZE(seg1), MAP_WRITE);
	test_assert(segs1 > 0);
	segs2 = check_filemap(inode, 2, 5, seg2, ARRAY_SIZE(seg2));
	test_assert(segs1 == segs2);

	/* Overwrite range */
	segs1 = d_filemaps(inode, 4, 1, seg1, ARRAY_SIZE(seg1), MAP_WRITE);
	test_assert(segs1 > 0);
	segs2 = check_filemap(inode, 4, 1, seg1, ARRAY_SIZE(seg1));
	test_assert(segs1 == segs2);
	test_assert(seg1[0].block == seg2[0].block + 2);

	/* Read 1st range again (overwrite should not add new segments) */
	segs1 = check_filemap(inode, 2, 5, seg1, ARRAY_SIZE(seg1));
	test_assert(segs1 == segs2);
	test_assert(seg1[0].block == seg2[0].block);
	test_assert(seg1[0].count == seg2[0].count);
	test_assert(seg1[0].count == 5);

	/* Check whole range from 0 */
	segs2 = check_filemap(inode, 0, 200, seg2, ARRAY_SIZE(seg2));
	test_assert(segs2 >= segs1);

	tux3_end_backend();

	/* Clear dirty page to prevent to call filemap again */
	change_begin_atomic(sb);
	truncate_inode_pages(mapping(inode), 0);
	change_end_atomic(sb);

	test_assert(force_delta(sb) == 0);
	clean_main(sb, inode);
}
TEST_DEFINE(TEST_UNIT, "test03", test03);

/* Test overwrite extent and hole at once */
static void test04(void *_arg)
{
	struct test_arg *arg = _arg;
	struct sb *sb = arg->sb;
	struct inode *inode = arg->inode;

	struct block_segment seg1[32], seg2[32];
	int segs1, segs2;

	/* Set fake backend mark to modify backend objects. */
	tux3_start_backend(sb);

	/* Create extents */
	segs1 = d_filemaps(inode, 2, 2, seg1, ARRAY_SIZE(seg1), MAP_WRITE);
	test_assert(segs1 > 0);
	segs2 = check_filemap(inode, 2, 2, seg2, ARRAY_SIZE(seg2));
	test_assert(segs1 == segs2);

	segs1 = d_filemaps(inode, 6, 2, seg1, ARRAY_SIZE(seg1), MAP_WRITE);
	test_assert(segs1 > 0);
	segs2 = check_filemap(inode, 6, 2, seg2, ARRAY_SIZE(seg2));
	test_assert(segs1 == segs2);

	/* Overwrite extent and hole at once [seg, hole, seg, hole] */
	segs1 = d_filemaps(inode, 2, 8, seg1, ARRAY_SIZE(seg1), MAP_WRITE);
	test_assert(segs1 > 0);
	segs2 = check_filemap(inode, 2, 8, seg2, ARRAY_SIZE(seg1));
	test_assert(segs1 == segs2);

	/* Check whole rage from 0 */
	segs2 = check_filemap(inode, 0, 200, seg2, ARRAY_SIZE(seg2));
	test_assert(segs2 >= segs1);

	tux3_end_backend();

	/* Clear dirty page to prevent to call filemap again */
	change_begin_atomic(sb);
	truncate_inode_pages(mapping(inode), 0);
	change_end_atomic(sb);

	test_assert(force_delta(sb) == 0);
	clean_main(sb, inode);
}
TEST_DEFINE(TEST_UNIT, "test04", test04);

static void __test05(struct test_data data[], int nr, struct inode *inode)
{
	struct test_data *t = data;
	struct block_segment seg[32];
	int total_segs = 0;

	for (int i = 0; i < nr; i++, t++) {
		int segs1, segs2;

		segs1 = d_filemaps(inode, t->index, t->count,
				   seg, ARRAY_SIZE(seg), t->mode);
		test_assert(segs1 > 0);
		total_segs += segs1;

		segs2 = check_filemap(inode, t->index, t->count,
				      seg, ARRAY_SIZE(seg));
		test_assert(segs1 == segs2);
	}
#if 0
	/* Check whole rage */
	block_t idx = data[0].index;
	unsigned end = data[nr - 1].index + data[nr - 1].count + 10;
	segs = check_filemap(inode, idx, end - idx, seg, ARRAY_SIZE(seg));
	test_assert(segs >= total_segs);
#endif
}

/* Test to write block to hole */
static void test05(void *_arg)
{
	struct test_arg *arg = _arg;
	struct sb *sb = arg->sb;
	struct inode *inode = arg->inode;

	struct test_data data[][3] = {
		/* Test case 1 */
		{
			{ .index = 2, .count = 1, .mode = MAP_WRITE, },
			{ .index = 6, .count = 1, .mode = MAP_WRITE, },
			{ .index = 4, .count = 1, .mode = MAP_WRITE, },
		},
		/* Test case 2 */
		{
			{ .index = 0x1100000, .count = 0x40, .mode=MAP_WRITE, },
			{ .index =  0x800000, .count = 0x40, .mode=MAP_WRITE, },
			{ .index =  0x800040, .count = 0x40, .mode=MAP_WRITE, },
		},
	};

	/* Set fake backend mark to modify backend objects. */
	tux3_start_backend(sb);

	for (int test = 0; test < ARRAY_SIZE(data); test++) {
		__test05(data[test], ARRAY_SIZE(data[test]), inode);

		int err = btree_chop(&tux_inode(inode)->btree, 0, TUXKEY_LIMIT);
		test_assert(!err);
	}

	tux3_end_backend();

	test_assert(force_delta(sb) == 0);
	clean_main(sb, inode);
}
TEST_DEFINE(TEST_UNIT, "test05", test05);

/* Test of filemap_hole stuff */
static void test06(void *_arg)
{
	struct test_arg *arg = _arg;
	struct sb *sb = arg->sb;
	struct inode *inode = arg->inode;

	struct file file = FILE_INIT(inode, 0);
	char *buf;
	int ret;

	int size = sb->blocksize * 10;
	buf = malloc(size);
	assert(buf);
	memset(buf, 'a', size);

	/* Prevent flush */
	change_begin(sb, 0); /* ignore ENOSPC */

	/* Write 10 blocks */
	ret = tuxio(&file, buf, size, 1);
	test_assert(ret == size);

	/* Make hole extent at block-9 */
	ret = __tuxtruncate(inode, sb->blocksize * 9);
	test_assert(ret == 0);
	test_assert(tux3_is_hole(inode, 8, 1) == 0);
	test_assert(tux3_is_hole(inode, 9, 1) == 1);
	/* Make hole extent at block-8 */
	ret = __tuxtruncate(inode, sb->blocksize * 8);
	test_assert(ret == 0);
	test_assert(tux3_is_hole(inode, 7, 1) == 0);
	test_assert(tux3_is_hole(inode, 8, 1) == 1);
	/* Make hole extent at block-2 */
	ret = __tuxtruncate(inode, sb->blocksize * 2);
	test_assert(ret == 0);
	test_assert(tux3_is_hole(inode, 1, 1) == 0);
	test_assert(tux3_is_hole(inode, 2, 1) == 1);

	char *tmp = malloc(size);
	assert(tmp);
	for (int i = 0; i < size / sb->blocksize; i++) {
		struct buffer_head *buffer = blockread(mapping(inode), i);
		test_assert(buffer);
		void *data = bufdata(buffer);
		if (i < 2)
			memset(tmp, 'a', sb->blocksize);
		else
			memset(tmp, '\0', sb->blocksize);
		test_assert(memcmp(data, tmp, sb->blocksize) == 0);
		blockput(buffer);
	}
	free(tmp);

	/* TODO: test of tux3_map_hole() */

	change_end(sb);
	free(buf);

	/* For tux3_clear_hole() */
	iput(inode);
	ret = tuxunlink(sb->rootdir, FOO, strlen(FOO));
	test_assert(ret == 0);

	test_assert(force_delta(sb) == 0);
	clean_main(sb, NULL);
}
TEST_DEFINE(TEST_UNIT, "test06", test06);

int main(int argc, char *argv[])
{
	int argi = test_init(argc, argv);

	if (argi >= argc)
		error_exit("usage: %s <volname>", argv[0]);

	char *name = argv[argi];
	int fd = open(name, O_CREAT|O_TRUNC|O_RDWR, S_IRUSR|S_IWUSR);
	u64 size = 1 << 24;
	assert(!ftruncate(fd, size));

	int err = tux3_init_mem(1 << 20, 2);
	assert(!err);

	struct dev *dev = &(struct dev){ .fd = fd, .bits = 8 };
	struct sb *sb = rapid_sb(dev);
	sb->super = INIT_DISKSB(dev->bits, size >> dev->bits);

	test_assert(mkfs_tux3(sb) == 0);

	struct tux_iattr iattr = { .mode = S_IFREG | 0644, };
	struct inode *inode = tuxcreate(sb->rootdir, FOO, strlen(FOO), &iattr);

	test_assert(force_unify(sb) == 0);

	struct test_arg arg = {
		.sb = sb,
		.inode = inode,
	};
	test_run(&arg);

	clean_main(sb, inode);
	return test_failures();
}
