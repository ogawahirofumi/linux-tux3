/*
 * Inode btree leaf operations
 *
 * Original copyright (c) 2008 Daniel Phillips <phillips@phunq.net>
 * Licensed under the GPL version 3
 *
 * By contributing changes to this file you grant the original copyright holder
 * the right to distribute those changes under any license.
 */

#include "tux3user.h"
#include "test.h"

#ifndef trace
#define trace trace_off
#endif

#include "../ileaf.c"

static void clean_main(struct sb *sb)
{
	tux3_free_idefer_map(sb->idefer_map);
	tux3_exit_mem();
}

static struct ileaf *ileaf_create(struct btree *btree)
{
	struct ileaf *leaf = malloc(btree->sb->blocksize);
	assert(leaf);
	btree->ops->leaf_init(btree, leaf);
	return leaf;
}

static void ileaf_destroy(struct btree *btree, struct ileaf *leaf)
{
	assert(!ileaf_sniff(btree, leaf));
	free(leaf);
}

static void test_append(struct btree *btree, struct ileaf *leaf, inum_t inum,
			int more, char fill)
{
	u16 size = 0;
	(void *)ileaf_lookup(btree, leaf, inum, &size);
	char *attrs = ileaf_resize(btree, leaf, inum, size + more);
	memset(attrs, fill, size + more);
}

static void test_remove(struct btree *btree, struct ileaf *leaf, inum_t inum,
			int less)
{
	u16 size = 0;
	char *attrs = ileaf_lookup(btree, leaf, inum, &size);
	test_assert(attrs);
	attrs = ileaf_resize(btree, leaf, inum, size - less);
}

struct ileaf_data {
	inum_t inum;
	int size;
	unsigned char c;
	unsigned char buf[32];
};

static void check_ileaf_with_data(struct btree *btree, struct ileaf *ileaf,
				  struct ileaf_data *data, int nr_data)
{
	for (int i = 0; i < nr_data; i++) {
		void *attrs;
		u16 size;
		attrs = ileaf_lookup(btree, ileaf, data[i].inum, &size);
		if (data[i].size == 0)
			test_assert(attrs == NULL);
		else {
			test_assert(attrs);
			test_assert(size == data[i].size);
			test_assert(!memcmp(data[i].buf, attrs, data[i].size));
		}
	}
}

static inum_t test_split(struct btree *btree, inum_t hint,
			 struct ileaf *src, struct ileaf *dst,
			 struct ileaf_data *data, int nr_data)
{
	struct ileaf_data *src_data, *dst_data;
	int i, data_size = sizeof(*data) * nr_data;
	inum_t dst_base;

	dst_base = ileaf_split(btree, hint, src, dst);

	/* Prepare data for split */
	src_data = malloc(data_size);
	dst_data = malloc(data_size);
	test_assert(src_data && dst_data);
	memcpy(src_data, data, data_size);
	memcpy(dst_data, data, data_size);
	for (i = 0; i < nr_data; i++) {
		if (data[i].inum < dst_base)
			dst_data[i].size = 0;
		else
			src_data[i].size = 0;
	}

	/* Check src and dst */
	check_ileaf_with_data(btree, src, src_data, nr_data);
	check_ileaf_with_data(btree, dst, dst_data, nr_data);

	free(src_data);
	free(dst_data);

	return dst_base;
}

static void test_merge(struct btree *btree, struct ileaf *into,
		       struct ileaf *from, struct ileaf_data *data, int nr_data)
{
	ileaf_merge(btree, into, from);
	/* Check into */
	check_ileaf_with_data(btree, into, data, nr_data);
}

static int cmp_data(const void *p1, const void *p2)
{
	const struct ileaf_data *data1 = p1, *data2 = p2;
	if (data1->inum < data2->inum)
		return -1;
	else if (data1->inum > data2->inum)
		return 1;
	return 0;
}

/* Test basic ileaf operations */
static void test01(struct sb *sb, struct btree *btree)
{
	struct ileaf *leaf = ileaf_create(btree);
	struct ileaf *dest = ileaf_create(btree);
	unsigned more;

	struct ileaf_data data[] = {
		/* Append contiguous inums */
		{ .inum = 26, .size = 1, .c = 'a', },
		{ .inum = 27, .size = 2, .c = 'b', },
		{ .inum = 28, .size = 3, .c = 'c', },
		{ .inum = 29, .size = 4, .c = 'd', },
		/* Append closer than IBASE_FAR */
		{ .inum = 32, .size = 5, .c = 'e', },
		{ .inum = 33, .size = 6, .c = 'f', },
		/* Append far inums */
		{ .inum = 50, .size = 7, .c = 'g', },
		{ .inum = 51, .size = 8, .c = 'h', },

		/* Prepend contiguous inums */
		{ .inum = 25, .size = 9, .c = 'i', },
		{ .inum = 24, .size = 8, .c = 'j', },
		/* Prepend closer than IBASE_FAR */
		{ .inum = 20, .size = 7, .c = 'k', },
		/* Prepend far inums */
		{ .inum = 10, .size = 6, .c = 'l', },
		{ .inum = 11, .size = 5, .c = 'm', },
	};

	/* Init data[] */
	for (int i = 0; i < ARRAY_SIZE(data); i++)
		memset(data[i].buf, data[i].c, data[i].size);

	/* Add data */
	for (int i = 0; i < ARRAY_SIZE(data); i++) {
		if (data[i].size == 0)
			continue;
		test_append(btree, leaf, data[i].inum, data[i].size, data[i].c);
	}
	/* Check */
	check_ileaf_with_data(btree, leaf, data, ARRAY_SIZE(data));

	/* Shrink attribute */
	more = 3;
	data[3].size -= more;
	test_remove(btree, leaf, data[3].inum, more);
	/* Check */
	check_ileaf_with_data(btree, leaf, data, ARRAY_SIZE(data));

	/* Expend attribute */
	more = 3;
	memset(data[3].buf + data[3].size, data[3].c, more);
	data[3].size += more;
	test_append(btree, leaf, data[3].inum, more, data[3].c);
	/* Check */
	check_ileaf_with_data(btree, leaf, data, ARRAY_SIZE(data));

	/* Change attribute */
	more = 2;
	memset(data[0].buf, 'x', data[0].size + more);
	data[0].size += more;
	test_append(btree, leaf, data[0].inum, more, 'x');
	/* Check */
	check_ileaf_with_data(btree, leaf, data, ARRAY_SIZE(data));

	inum_t dest_base;
	/* Split leaf before first entry */
	dest_base = test_split(btree, 5, leaf, dest, data, ARRAY_SIZE(data));
	test_assert(dest_base == 5);
	/* Merge leaf and dest */
	test_merge(btree, leaf, dest, data, ARRAY_SIZE(data));

	/* Split leaf at after end */
	dest_base = test_split(btree, 60, leaf, dest, data, ARRAY_SIZE(data));
	test_assert(dest_base == 60);
	/* Merge leaf and dest */
	test_merge(btree, leaf, dest, data, ARRAY_SIZE(data));

	/* Split leaf at middle (ibase boundary) */
	dest_base = test_split(btree, 20, leaf, dest, data, ARRAY_SIZE(data));
	test_assert(dest_base == 20);
	/* Merge leaf and dest (don't merge ibase entries) */
	test_merge(btree, leaf, dest, data, ARRAY_SIZE(data));

	/* Split leaf at middle (not ibase boundary) */
	dest_base = test_split(btree, 27, leaf, dest, data, ARRAY_SIZE(data));
	test_assert(dest_base == 27);
	/* Merge leaf and dest (merge ibase entries) */
	test_merge(btree, leaf, dest, data, ARRAY_SIZE(data));

	/* Split leaf at middle (not ibase boundary, and split at hole) */
	dest_base = test_split(btree, 22, leaf, dest, data, ARRAY_SIZE(data));
	test_assert(dest_base == 22);
	/* Merge leaf and dest (merge ibase entries) */
	test_merge(btree, leaf, dest, data, ARRAY_SIZE(data));

	/* Split leaf at middle (not ibase boundary) */
	dest_base = test_split(btree, 32, leaf, dest, data, ARRAY_SIZE(data));
	test_assert(dest_base == 32);
	/* Merge leaf and dest (merge ibase entries, but have hole on ibases) */
	test_merge(btree, leaf, dest, data, ARRAY_SIZE(data));

	/* Split leaf at middle (not ibase boundary, and not dict entry) */
	dest_base = test_split(btree, 40, leaf, dest, data, ARRAY_SIZE(data));
	test_assert(dest_base == 40);
	/* Merge leaf and dest (merge ibase entries) */
	test_merge(btree, leaf, dest, data, ARRAY_SIZE(data));

	/* Test find_empty_inode() */
	qsort(data, ARRAY_SIZE(data), sizeof(data[0]), cmp_data);
	for (int i = 5; i <= 60; i++) {
		inum_t alloc;
		int ret = ileaf_find_free(btree, 0, TUXKEY_LIMIT, leaf,
					  i, TUXKEY_LIMIT, &alloc);
		test_assert(ret == 1);

		inum_t expected = i;
		for (int j = 0; j < ARRAY_SIZE(data); j++) {
			if (expected == data[j].inum)
				expected++;
		}

		test_assert(alloc == expected);
	}

	ileaf_destroy(btree, leaf);
	ileaf_destroy(btree, dest);

	clean_main(sb);
}

/* Test ileaf_split_hint */
static void test02(struct sb *sb, struct btree *btree)
{
	struct ileaf *leaf = ileaf_create(btree);
	tuxkey_t key, base, hint;
	int count, size = 64;

	/* hint must be valid range */
	for (base = 0; base < 100; base++) {
		leaf->ibase = cpu_to_be64(base);
		for (count = 1; count < 100; count++) {
			leaf->count = cpu_to_be16(count);
			for (key = base; key < base + count + 100; key++) {
				tuxkey_t limit = max(base + count, key + 1);

				hint = ileaf_split_hint(btree, base, limit,
							leaf, key, size);

				test_assert(base <= hint);
				test_assert(hint < limit);
			}
		}
	}

	ileaf_destroy(btree, leaf);
	clean_main(sb);
}

int main(int argc, char *argv[])
{
	struct dev *dev = &(struct dev){ .bits = 12 };

	int err = tux3_init_mem(1 << 20, 2);
	assert(!err);

	struct sb *sb = rapid_sb(dev);
	sb->super = INIT_DISKSB(dev->bits, 150);
	assert(!setup_sb(sb, &sb->super));

	struct btree btree;
	init_btree(&btree, sb, no_root, &itree_ops);

	test_init(argv[0]);

	if (test_start("test01"))
		test01(sb, &btree);
	test_end();

	if (test_start("test02"))
		test02(sb, &btree);
	test_end();

	clean_main(sb);
	return test_failures();
}
