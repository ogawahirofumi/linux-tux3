/*
 * btree microbench.
 */

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

static void bench01(struct sb *sb, struct inode *inode)
{
	struct btree *btree = &tux_inode(inode)->btree;
	int err;

	init_btree(btree, sb, no_root, &ops);
	/* Restrict max count of leaf entries, to make btree deep easily */
	tux_inode(inode)->btree.entries_per_leaf = 2;

	err = btree_alloc_empty(btree);
	test_assert(!err);

	struct cursor *cursor = alloc_cursor(btree, 9); /* +9 for new depth */
	test_assert(cursor);

	struct test_elapse elapse;
	struct timeval e;

	int keys = sb->bnode_max_count * sb->bnode_max_count * btree->entries_per_leaf;

	test_elapse_start(&elapse);
	for (int key = keys - 1; key >= 0; key--)
		bench_btree_write(cursor, key);
	e = test_elapse_stop(&elapse);
	printf("%s: insert %ld.%06ld secs\n", __func__, e.tv_sec, e.tv_usec);

	test_elapse_start(&elapse);
	for (int key = 0; key < keys; key++) {
		btree_probe(cursor, key);
		release_cursor(cursor);
	}
	e = test_elapse_stop(&elapse);
	printf("%s: probe %ld.%06ld secs\n", __func__, e.tv_sec, e.tv_usec);

	test_elapse_start(&elapse);
	for (int key = 0; key < keys; key++)
		btree_chop(btree, key, 1);
	e = test_elapse_stop(&elapse);
	printf("%s: chop %ld.%06ld secs\n", __func__, e.tv_sec, e.tv_usec);

	clean_main(sb, inode);
}

static void bench(int argc, char *argv[])
{
	if (1)
		return;

	test_init(argc, argv);

	unsigned long max_mem_size = 1 << 30;
	struct dev *dev = &(struct dev){ .bits = 12 };

	int err = tux3_init_mem(max_mem_size, 2);
	assert(!err);

	struct sb *sb = rapid_sb(dev);
	sb->super = INIT_DISKSB(dev->bits, max_mem_size >> dev->bits);
	assert(!setup_sb(sb, &sb->super));
	assert(!set_blocksize(sb->blocksize));

	sb->volmap = tux_new_volmap(sb);
	assert(sb->volmap);
	sb->logmap = tux_new_logmap(sb);
	assert(sb->logmap);

	struct inode *inode = rapid_new_inode(sb, dev_errio, 0);
	assert(inode);

	/* Set fake backend mark to modify backend objects. */
	tux3_start_backend(sb);

	bench01(sb, inode);

	exit(0);
}
