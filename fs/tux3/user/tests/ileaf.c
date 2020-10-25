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

struct test_arg {
	struct sb *sb;
	struct btree *btree;
};

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
static void test01(void *_arg)
{
	struct test_arg *arg = _arg;
	struct sb *sb = arg->sb;
	struct btree *btree = arg->btree;

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
TEST_DEFINE(TEST_UNIT, "test01", test01);

/* Test merge by ileaf_resize */
static void test02(void *_arg)
{
	struct test_arg *arg = _arg;
	struct sb *sb = arg->sb;
	struct btree *btree = arg->btree;

	struct ileaf *leaf = ileaf_create(btree);

	struct ileaf_data data[] = {
		/* Append contiguous inums */
		{ .inum = 20, .size = 1, .c = 'a', },
		{ .inum = 21, .size = 2, .c = 'b', },
		{ .inum = 22, .size = 3, .c = 'c', },
		{ .inum = 23, .size = 4, .c = 'd', },
		{ .inum = 24, .size = 5, .c = 'e', },

		/* Append can merge entries */
		{ .inum = 34, .size = 9, .c = 'f', },
		{ .inum = 33, .size = 8, .c = 'g', },
		{ .inum = 32, .size = 7, .c = 'h', },
		{ .inum = 31, .size = 6, .c = 'i', },
		{ .inum = 30, .size = 5, .c = 'j', },
		{ .inum = 29, .size = 4, .c = 'k', },
		{ .inum = 28, .size = 3, .c = 'l', },
		{ .inum = 27, .size = 2, .c = 'm', },
		{ .inum = 26, .size = 1, .c = 'n', },
		{ .inum = 25, .size = 9, .c = 'o', },

		/* Prepend can merge entries */
		{ .inum = 10, .size = 1, .c = 'p', },
		{ .inum = 11, .size = 2, .c = 'q', },
		{ .inum = 12, .size = 3, .c = 'r', },
		{ .inum = 13, .size = 4, .c = 's', },
		{ .inum = 14, .size = 5, .c = 't', },
		{ .inum = 15, .size = 6, .c = 'u', },
		{ .inum = 16, .size = 7, .c = 'v', },
		{ .inum = 17, .size = 8, .c = 'w', },
		{ .inum = 18, .size = 9, .c = 'x', },
		{ .inum = 19, .size = 1, .c = 'y', },
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
#ifdef ILEAF_FORMAT_MULTI_IBASE
	test_assert(ibase_count(leaf) == 1);
#endif
	check_ileaf_with_data(btree, leaf, data, ARRAY_SIZE(data));

	ileaf_destroy(btree, leaf);

	clean_main(sb);
}
TEST_DEFINE(TEST_UNIT, "test02", test02);

static void test_split_hint(struct btree *btree, inum_t key_bottom,
			    inum_t key_limit, struct ileaf *ileaf,
			    inum_t key, int size, inum_t ibase, int count)
{
	inum_t hint;

#ifndef ILEAF_FORMAT_MULTI_IBASE
	ileaf->ibase = cpu_to_be64(ibase);
	ileaf->count = cpu_to_be16(count);
#else
	ileaf->ibase_count = cpu_to_be16(1);
	ibase_write(ileaf->head, ibase);
	ibase_dictend_write(ileaf->head, count);
#endif

	hint = ileaf_split_hint(btree, key_bottom, key_limit, ileaf, key, size);

	test_assert(key_bottom < hint);
	test_assert(hint < key_limit);
}

/* Test ileaf_split_hint */
static void test03(void *_arg)
{
	struct test_arg *arg = _arg;
	struct sb *sb = arg->sb;
	struct btree *btree = arg->btree;

	struct ileaf *leaf = ileaf_create(btree);
	tuxkey_t key, ibase, bottom, limit;
	int count, size = 64;

	/* hint must be valid range (range is almost full) */
	for (ibase = 0; ibase < 100; ibase++) {
		for (count = 2; count < 100; count++) {
			for (key = ibase; key < ibase + count + 100; key++) {
				limit = max(ibase + count, key + 1);
				test_split_hint(btree, ibase, limit, leaf,
						key, size, ibase, count);
			}
		}
	}

	/* hint must be valid range (there is heading space) */
	for (ibase = 10000; ibase < 10100; ibase += 10) {
		for (count = 1; count < 100; count++) {
			for (key = 0; key < ibase; key += 1000) {
				for (bottom = 0; bottom < 1000; bottom += 1000){
					bottom = min(key, bottom);
					limit = ibase + count;
					test_split_hint(btree, bottom, limit,
							leaf, key, size,
							ibase, count);
				}
			}
		}
	}

	/* hint must be valid range (there is trailing space) */
	for (ibase = 10000; ibase < 10100; ibase += 10) {
		for (count = 1; count < 100; count++) {
			inum_t end = ibase + count;
			for (key = end; key < end + 10000; key += 1000) {
				bottom = ibase;
				limit = end + 10000;
				test_split_hint(btree, bottom, limit,
						leaf, key, size,
						ibase, count);
			}
		}
	}

	/* hint must be valid range (there is headling/trailing space) */
	ibase = 10000;
	for (count = 1; count < 100; count++) {
		for (bottom = ibase - 10; bottom < ibase; bottom++) {
			inum_t end = ibase + count;
			for (limit = end; limit < end + 10; limit++) {
				for (key = bottom; key < end; key++) {
					/* if count==1, should be key!=ibase */
					if (count == 1 && key == ibase)
						continue;

					test_split_hint(btree, bottom, limit,
							leaf, key, size, ibase,
							count);
				}
			}
		}
	}

	ileaf_destroy(btree, leaf);
	clean_main(sb);
}
TEST_DEFINE(TEST_UNIT, "test03", test03);

static void test_ileaf_chop(struct btree *btree, tuxkey_t start, u64 len,
			    struct ileaf *ileaf,
			    struct ileaf_data *data, int nr_data)
{
	int i, chopped, ret;

	/* Set size=0 on chop range */
	chopped = 0;
	for (i = 0; i < nr_data; i++) {
		if (start <= data[i].inum && data[i].inum < start + len) {
			data[i].size = 0;
			chopped=1;
		}
	}

	ret = ileaf_chop(btree, start, len, ileaf);
	test_assert(ret == chopped);
	check_ileaf_with_data(btree, ileaf, data, nr_data);
}

/* Test of ileaf_chop */
static void test04(void *_arg)
{
	struct test_arg *arg = _arg;
	struct sb *sb = arg->sb;
	struct btree *btree = arg->btree;

	struct ileaf *leaf = ileaf_create(btree);

	struct ileaf_data data[] = {
		{ .inum = 20, .size = 9, .c = 'a', },
		{ .inum = 21, .size = 8, .c = 'b', },
		{ .inum = 22, .size = 7, .c = 'c', },
		{ .inum = 23, .size = 6, .c = 'd', },
		{ .inum = 24, .size = 5, .c = 'e', },
		{ .inum = 25, .size = 4, .c = 'f', },
		{ .inum = 26, .size = 3, .c = 'g', },
		{ .inum = 27, .size = 2, .c = 'h', },
		{ .inum = 28, .size = 1, .c = 'i', },
		{ .inum = 29, .size = 3, .c = 'j', },

		{ .inum = 10, .size = 2, .c = 'k', },
		{ .inum = 11, .size = 3, .c = 'l', },
		{ .inum = 12, .size = 4, .c = 'm', },
		{ .inum = 13, .size = 5, .c = 'n', },
		{ .inum = 14, .size = 6, .c = 'o', },
		{ .inum = 15, .size = 7, .c = 'p', },
		{ .inum = 16, .size = 8, .c = 'q', },
		{ .inum = 17, .size = 9, .c = 'r', },

		{ .inum = 40, .size = 4, .c = 's', },
		{ .inum = 41, .size = 4, .c = 't', },
		{ .inum = 42, .size = 4, .c = 'u', },
		{ .inum = 43, .size = 4, .c = 'v', },

		{ .inum = 60, .size = 4, .c = 'w', },
		{ .inum = 65, .size = 4, .c = 'x', },
		{ .inum = 70, .size = 4, .c = 'y', },
		{ .inum = 75, .size = 4, .c = 'z', },
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

	if (test_start("test04.0")) {
		/* Chop before start */
		test_ileaf_chop(btree, 5, 2, leaf, data, ARRAY_SIZE(data));
		/* Chop at hole */
		test_ileaf_chop(btree, 30, 2, leaf, data, ARRAY_SIZE(data));
		/* Chop after end */
		test_ileaf_chop(btree, 80, 2, leaf, data, ARRAY_SIZE(data));
		ileaf_destroy(btree, leaf);
		clean_main(sb);
	}
	test_end();

	if (test_start("test04.1.1")) {
		/* !s_partial, not across ibase entries, !e_partial */
		test_ileaf_chop(btree, 10, 8, leaf, data, ARRAY_SIZE(data));
		ileaf_destroy(btree, leaf);
		clean_main(sb);
	}
	test_end();
	if (test_start("test04.1.2")) {
		/* !s_partial, not across ibase entries, !e_partial */
		test_ileaf_chop(btree, 20, 10, leaf, data, ARRAY_SIZE(data));
		ileaf_destroy(btree, leaf);
		clean_main(sb);
	}
	test_end();
	if (test_start("test04.1.3")) {
		/* !s_partial, not across ibase entries, !e_partial */
		test_ileaf_chop(btree, 40, 4, leaf, data, ARRAY_SIZE(data));
		ileaf_destroy(btree, leaf);
		clean_main(sb);
	}
	test_end();
	if (test_start("test04.1.4")) {
		/* !s_partial, across ibase entries, !e_partial */
		test_ileaf_chop(btree, 20, 24, leaf, data, ARRAY_SIZE(data));
		ileaf_destroy(btree, leaf);
		clean_main(sb);
	}
	test_end();

	if (test_start("test04.2.1")) {
		/* s_partial, not across ibase entries, !e_partial */
		test_ileaf_chop(btree, 27, 3, leaf, data, ARRAY_SIZE(data));
		ileaf_destroy(btree, leaf);
		clean_main(sb);
	}
	test_end();
	if (test_start("test04.2.2")) {
		/* s_partial, across ibase entries, !e_partial */
		test_ileaf_chop(btree, 27, 17, leaf, data, ARRAY_SIZE(data));
		ileaf_destroy(btree, leaf);
		clean_main(sb);
	}
	test_end();

	if (test_start("test04.3.1")) {
		/* start < ibase, not across ibase entries, e_partial */
		test_ileaf_chop(btree, 5, 8, leaf, data, ARRAY_SIZE(data));
		ileaf_destroy(btree, leaf);
		clean_main(sb);
	}
	test_end();
	if (test_start("test04.3.2")) {
		/* !s_partial, not across ibase entries, e_partial */
		test_ileaf_chop(btree, 20, 5, leaf, data, ARRAY_SIZE(data));
		ileaf_destroy(btree, leaf);
		clean_main(sb);
	}
	test_end();
	if (test_start("test04.3.3")) {
		/* !s_partial, not across ibase entries, e_partial */
		test_ileaf_chop(btree, 20, 23, leaf, data, ARRAY_SIZE(data));
		ileaf_destroy(btree, leaf);
		clean_main(sb);
	}
	test_end();

	if (test_start("test04.4.1")) {
		/* s_partial, not across ibase entries, e_partial */
		/* chop dict is close (len < IBASE_FAR) */
		test_ileaf_chop(btree, 23, 3, leaf, data, ARRAY_SIZE(data));
		ileaf_destroy(btree, leaf);
		clean_main(sb);
	}
	test_end();
	if (test_start("test04.4.2")) {
		/* s_partial, across ibase entries, e_partial */
		/* chop dict is close (len < IBASE_FAR) */
		test_ileaf_chop(btree, 17, 4, leaf, data, ARRAY_SIZE(data));
		ileaf_destroy(btree, leaf);
		clean_main(sb);
	}
	test_end();
	if (test_start("test04.4.3")) {
		/* s_partial, not across ibase entries, e_partial */
		/* chop dict is far (len >= IBASE_FAR) */
		test_ileaf_chop(btree, 21, 8, leaf, data, ARRAY_SIZE(data));
		ileaf_destroy(btree, leaf);
		clean_main(sb);
	}
	test_end();
	if (test_start("test04.4.3")) {
		/* s_partial, across ibase entries, e_partial */
		/* chop dict is far (len >= IBASE_FAR) */
		test_ileaf_chop(btree, 21, 22, leaf, data, ARRAY_SIZE(data));
		ileaf_destroy(btree, leaf);
		clean_main(sb);
	}
	test_end();

	if (test_start("test04.5")) {
		/* s_partial, around of 0-size entries, e_partial */
		/* chop dict is far (len >= IBASE_FAR) */
		test_ileaf_chop(btree, 65, 6, leaf, data, ARRAY_SIZE(data));
		ileaf_destroy(btree, leaf);
		clean_main(sb);
	}
	test_end();

	if (test_start("test04.6")) {
		/* s_partial, not across ibase entries, beyond end */
		test_ileaf_chop(btree, 70, 10, leaf, data, ARRAY_SIZE(data));
		ileaf_destroy(btree, leaf);
		clean_main(sb);
	}
	test_end();

	test_ileaf_chop(btree, 22, 3, leaf, data, ARRAY_SIZE(data));
	test_ileaf_chop(btree, 17, 4, leaf, data, ARRAY_SIZE(data));
	test_ileaf_chop(btree, 21, 8, leaf, data, ARRAY_SIZE(data));
	test_ileaf_chop(btree, 17, 23, leaf, data, ARRAY_SIZE(data));

	ileaf_destroy(btree, leaf);

	clean_main(sb);
}
TEST_DEFINE(TEST_UNIT, "test04", test04);

/* Test of ileaf_chop and trim tail */
static void test05(void *_arg)
{
	struct test_arg *arg = _arg;
	struct sb *sb = arg->sb;
	struct btree *btree = arg->btree;

	struct ileaf *leaf = ileaf_create(btree);

	struct ileaf_data data[] = {
		{ .inum = 10, .size = 2, .c = 'k', },
		{ .inum = 11, .size = 3, .c = 'l', },
		{ .inum = 12, .size = 4, .c = 'm', },
		{ .inum = 13, .size = 5, .c = 'n', },
		{ .inum = 14, .size = 6, .c = 'o', },
		{ .inum = 15, .size = 7, .c = 'p', },
		{ .inum = 16, .size = 8, .c = 'q', },
		{ .inum = 17, .size = 9, .c = 'r', },

		{ .inum = 40, .size = 4, .c = 's', },
		{ .inum = 41, .size = 4, .c = 't', },
		{ .inum = 42, .size = 4, .c = 'u', },
		{ .inum = 43, .size = 4, .c = 'v', },
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

	/* Chop after hole */
	test_ileaf_chop(btree, 40, 4, leaf, data, ARRAY_SIZE(data));

	/* Check if trim tail is working */
	inum_t last_inum;
#ifndef ILEAF_FORMAT_MULTI_IBASE
	last_inum = ileaf_ibase(leaf) + ileaf_count(leaf) - 1;
#else
	test_assert(ibase_count(leaf) == 1);
	last_inum = ibase_read(leaf->head) + ibase_dictend_read(leaf->head) - 1;
#endif
	test_assert(last_inum == 17);

	ileaf_destroy(btree, leaf);
	clean_main(sb);
}
TEST_DEFINE(TEST_UNIT, "test05", test05);

struct enum_data {
	struct ileaf_data *data;
	int nr;
};
static int enum_callback(struct btree *btree, inum_t inum, void *attrs,
			 unsigned size, void *data)
{
	struct enum_data *enum_data = data;
	int i;

	for (i = 0; i < enum_data->nr; i++) {
		if (inum == enum_data->data[i].inum) {
			struct ileaf_data *d = &enum_data->data[i];
			test_assert(size == d->size);
			test_assert(!memcmp(d->buf, attrs, size));
			/* Set 0-size to tell hit */
			d->size = 0;
			break;
		}
	}

	return 0;
}

static void test_enum(struct ileaf_data *data, int nr, inum_t start, u64 len)
{
	int i;

	for (i = 0; i < nr; i++) {
		if (start <= data[i].inum && data[i].inum < start + len)
			test_assert(data[i].size == 0);
		else
			test_assert(data[i].size > 0);
	}
}

/* Test of ileaf_enumerate */
static void test06(void *_arg)
{
	struct test_arg *arg = _arg;
	struct sb *sb = arg->sb;
	struct btree *btree = arg->btree;

	struct ileaf *leaf = ileaf_create(btree);

	struct ileaf_data data[] = {
		{ .inum = 20, .size = 9, .c = 'a', },
		{ .inum = 21, .size = 8, .c = 'b', },
		{ .inum = 22, .size = 7, .c = 'c', },
		{ .inum = 23, .size = 6, .c = 'd', },
		{ .inum = 24, .size = 5, .c = 'e', },
		{ .inum = 25, .size = 4, .c = 'f', },
		{ .inum = 26, .size = 3, .c = 'g', },
		{ .inum = 27, .size = 2, .c = 'h', },
		{ .inum = 28, .size = 1, .c = 'i', },
		{ .inum = 29, .size = 3, .c = 'j', },

		{ .inum = 10, .size = 2, .c = 'k', },
		{ .inum = 11, .size = 3, .c = 'l', },
		{ .inum = 12, .size = 4, .c = 'm', },
		{ .inum = 13, .size = 5, .c = 'n', },
		{ .inum = 14, .size = 6, .c = 'o', },
		{ .inum = 15, .size = 7, .c = 'p', },
		{ .inum = 16, .size = 8, .c = 'q', },
		{ .inum = 17, .size = 9, .c = 'r', },

		{ .inum = 40, .size = 4, .c = 's', },
		{ .inum = 41, .size = 4, .c = 't', },
		{ .inum = 42, .size = 4, .c = 'u', },
		{ .inum = 43, .size = 4, .c = 'v', },

		{ .inum = 60, .size = 4, .c = 'w', },
		{ .inum = 65, .size = 4, .c = 'x', },
		{ .inum = 70, .size = 4, .c = 'y', },
		{ .inum = 75, .size = 4, .c = 'z', },
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

	struct enum_data enum_data = {
		.data	= data,
		.nr	= ARRAY_SIZE(data),
	};
	struct ileaf_enumrate_cb cb = {
		.callback	= enum_callback,
		.data		= &enum_data,
	};

	if (test_start("test06.1")) {
		/* Outside inum */
		int ret = ileaf_enumerate(btree, 0, TUXKEY_LIMIT, leaf,
					  100, TUXKEY_LIMIT, &cb);
		test_assert(ret == 0);
		/* Should be no hit */
		test_enum(data, ARRAY_SIZE(data), 100, TUXKEY_LIMIT - 100);

		ileaf_destroy(btree, leaf);
		clean_main(sb);
	}
	test_end();

	if (test_start("test06.2")) {
		/* Enumerate whole inums */
		int ret = ileaf_enumerate(btree, 0, TUXKEY_LIMIT, leaf,
					  0, TUXKEY_LIMIT, &cb);
		test_assert(ret == 0);
		/* Should be hit all */
		test_enum(data, ARRAY_SIZE(data), 0, TUXKEY_LIMIT);

		ileaf_destroy(btree, leaf);
		clean_main(sb);
	}
	test_end();

	if (test_start("test06.3")) {
		/* Enumerate inums from hole */
		int ret = ileaf_enumerate(btree, 0, TUXKEY_LIMIT, leaf,
					  38, TUXKEY_LIMIT, &cb);
		test_assert(ret == 0);
		/* Should be partial hit */
		test_enum(data, ARRAY_SIZE(data), 38, TUXKEY_LIMIT - 38);

		ileaf_destroy(btree, leaf);
		clean_main(sb);
	}
	test_end();

	if (test_start("test06.4")) {
		/* Enumerate inums from middle of ibase */
		int ret = ileaf_enumerate(btree, 0, TUXKEY_LIMIT, leaf,
					  13, TUXKEY_LIMIT, &cb);
		test_assert(ret == 0);
		/* Should be partial hit */
		test_enum(data, ARRAY_SIZE(data), 13, TUXKEY_LIMIT - 13);

		ileaf_destroy(btree, leaf);
		clean_main(sb);
	}
	test_end();

	if (test_start("test06.5")) {
		/* Enumerate inums, but limit by length (end is hole) */
		int ret = ileaf_enumerate(btree, 0, TUXKEY_LIMIT, leaf,
					  0, 19, &cb);
		test_assert(ret == 0);
		/* Should be partial hit */
		test_enum(data, ARRAY_SIZE(data), 0, 19);

		ileaf_destroy(btree, leaf);
		clean_main(sb);
	}
	test_end();

	if (test_start("test06.6")) {
		/* Enumerate inums, but limit by length (end is not hole) */
		int ret = ileaf_enumerate(btree, 0, TUXKEY_LIMIT, leaf,
					  0, 22, &cb);
		test_assert(ret == 0);
		/* Should be partial hit */
		test_enum(data, ARRAY_SIZE(data), 0, 22);

		ileaf_destroy(btree, leaf);
		clean_main(sb);
	}
	test_end();

	ileaf_destroy(btree, leaf);
	clean_main(sb);
}
TEST_DEFINE(TEST_UNIT, "test06", test06);

int main(int argc, char *argv[])
{
	test_init(argc, argv);

	struct dev *dev = &(struct dev){ .bits = 12 };

	int err = tux3_init_mem(1 << 20, 2);
	assert(!err);

	struct sb *sb = rapid_sb(dev);
	sb->super = INIT_DISKSB(dev->bits, 150);
	assert(!setup_sb(sb, &sb->super));

	struct btree btree;
	init_btree(&btree, sb, no_root, &itree_ops);

	struct test_arg arg = {
		.sb = sb,
		.btree = &btree,
	};
	test_run(&arg);

	clean_main(sb);
	return test_failures();
}
