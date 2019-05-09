/*
 * btree microbench.
 */

#include "bench.h"

struct bench_arg {
	struct cursor *cursor;
	tuxkey_t keys;
};

static void bench_btree_write(struct cursor *cursor, tuxkey_t key)
{
	int err;

	err = btree_probe(cursor, key);
	test_assert(!err);

	struct uleaf_req rq = {
		.key = {
			.start	= key,
			.len	= 1,
		},
		.val		= key + 0x100,
	};
	err = btree_write(cursor, &rq.key);
	test_assert(!err);

	release_cursor(cursor);
}

static void bench01_write(void *_arg, int i)
{
	struct bench_arg *arg = _arg;
	struct cursor *cursor = arg->cursor;
	tuxkey_t key = arg->keys - 1 - i;
	bench_btree_write(cursor, key);
}

static void bench01_probe(void *_arg, int i)
{
	struct cursor *cursor = _arg;
	btree_probe(cursor, i);
}
static void bench01_probe_post(void *_arg, int i)
{
	struct cursor *cursor = _arg;
	release_cursor(cursor);
}

static void bench01_chop(void *_arg, int i)
{
	struct btree *btree = _arg;
	btree_chop(btree, i, 1);
}

static void bench01(void *_arg)
{
	struct test_arg *arg = _arg;
	struct sb *sb = arg->sb;
	struct inode *inode = arg->inode;
	struct btree *btree = &tux_inode(inode)->btree;
	int err;

	init_btree(btree, sb, no_root, &ops);
	/* Restrict max count of leaf entries, to make btree deep easily */
	tux_inode(inode)->btree.entries_per_leaf = 2;

	err = btree_alloc_empty(btree);
	test_assert(!err);

	struct cursor *cursor = alloc_cursor(btree, 9); /* +9 for new depth */
	test_assert(cursor);

	int keys = sb->bnode_max_count * sb->bnode_max_count * btree->entries_per_leaf;

	bench_measure_overhead();

	struct bench bench_write = {
		.name = "write",
		.test = bench01_write,
	};
	struct bench_arg bench_arg = {
		.cursor = cursor,
		.keys = keys,
	};
	bench_run(&bench_write, keys, &bench_arg);

	/* depending on bench_write makes data to probe */
	struct bench bench_probe = {
		.name = "probe",
		.test = bench01_probe,
		.post = bench01_probe_post,
	};
	bench_run(&bench_probe, keys, cursor);

	/* depending on bench_write makes data to chop */
	struct bench bench_chop = {
		.name = "chop",
		.test = bench01_chop,
	};
	bench_run(&bench_chop, keys, btree);

	clean_main(sb, inode);
}
TEST_DEFINE(TEST_BENCH, "bench01", bench01);
