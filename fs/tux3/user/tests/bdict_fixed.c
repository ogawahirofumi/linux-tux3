/*
 * Fixed bdict tests
 */

#include "tux3user.h"
#include "test.h"

#ifndef trace
#define trace trace_off
#endif

static inline int dict_key_compare(const void *__p1, const void *__p2)
{
	u64 p1 = get_unaligned_be64(__p1);
	u64 p2 = get_unaligned_be64(__p2);

	if (p1 < p2)
		return -1;
	else if (p1 > p2)
		return 1;
	return 0;
}

#define FBDICT_BIG_ENDIAN	true
#define FBDICT_KEY_SIZE		sizeof(__be64)
#define FBDICT_DATA_SIZE	sizeof(u64)
#define FBDICT_COMPARE(k, p)	dict_key_compare(k, p)
#define FBDICT_ZERO_CLEAR	true
#define FBDICT_NEED_SPLIT	true
#define FBDICT_NEED_MERGE	true
#include "../bdict_fixed.c"

struct test_data {
	__be64 key;
	u64 data;
};

static inline __be64 *test_keyp(void *buf, int idx)
{
	return fbdict_keyp(buf, idx);
}
static inline __be64 test_key_be64(void *buf, int idx)
{
	return get_unaligned(test_keyp(buf, idx));
}
static inline u64 test_key(void *buf, int idx)
{
	return get_unaligned_be64(test_keyp(buf, idx));
}
static inline u64 *test_datap(void *buf, int size, int idx)
{
	return fbdict_datap(buf, size, idx);
}
static inline u64 test_data(void *buf, int size, int idx)
{
	return *test_datap(buf, size, idx);
}

static bool check_key_data(struct test_data *t, void *buf, int blocksize, int i)
{
	__be64 k = test_key_be64(buf, i);
	u64 p = test_data(buf, blocksize, i);
	return t->key == k && t->data == p;
}

static void test01(int blocksize)
{
	void *buf = malloc(blocksize);
	int max_count = fbdict_max_count(blocksize);

	fbdict_init(buf, blocksize);

	struct test_data *tests = malloc(max_count * sizeof(struct test_data));
	for (int i = 0; i < max_count; i++) {
		tests[i].key = cpu_to_be64(5 + i * 2);
		tests[i].data = i;
	}

	/* Insert entries */
	for (int idx = 0; idx < max_count; idx++) {
		__be64 key = tests[idx].key;
		u64 data = tests[idx].data;

		u64 *p = fbdict_insert(buf, blocksize, max_count, idx, &key);
		test_assert(p != NULL);
		test_assert((void *)p > buf);
		test_assert((void *)p < buf + blocksize);
		*p = data;
	}

	/* Check inserted entries */
	for (int idx = 0; idx < fbdict_count(buf); idx++)
		check_key_data(&tests[idx], buf, blocksize, idx);

	for (int i = 0; i < fbdict_count(buf); i++) {
		__be64 key = tests[i].key;
		int idx = fbdict_lookup(buf, &key, 0);
		test_assert(idx == i);
		check_key_data(&tests[idx], buf, blocksize, idx);
	}

	{
		__be64 key;
		int idx;
		/* key==0 is smaller than istart=1 */
		key = tests[0].key;
		idx = fbdict_lookup(buf, &key, 1);
		test_assert(idx == -1);
		/* istart is over bd_count */
		key = tests[max_count - 1].key;
		be64_add_cpu(&key, 5);
		idx = fbdict_lookup(buf, &key, max_count);
		test_assert(idx == -1);
		/* key==0 is smaller than all keys */
		key = cpu_to_be64(0);
		idx = fbdict_lookup(buf, &key, 0);
		test_assert(idx == -1);

		/* Split and check */
		int split_idx = 6, count = fbdict_count(buf);
		void *buf2 = malloc(blocksize);
		fbdict_split(buf, blocksize, split_idx, buf2);

		test_assert(fbdict_count(buf) == split_idx);
		for (int i = 0; i < split_idx; i++)
			check_key_data(&tests[i], buf, blocksize, i);
		test_assert(fbdict_count(buf2) == count - split_idx);
		for (int i = split_idx; i < count; i++)
			check_key_data(&tests[i], buf, blocksize, i -split_idx);

		/* Merge and check */
		bool done = fbdict_merge(buf, blocksize, max_count, buf2);
		test_assert(done);
		for (int i = 0; i < fbdict_count(buf); i++)
			check_key_data(&tests[i], buf, blocksize, i);

		free(buf2);
	}

	/* Delete some idx, then key is not found */
	int delete_idx[] = { 5, 8, };
	for (int i = 0; i < ARRAY_SIZE(delete_idx); i++) {
		int count = fbdict_count(buf);
		__be64 key = test_key_be64(buf, delete_idx[i]);
		fbdict_delete(buf, blocksize, delete_idx[i], 1);
		test_assert(fbdict_count(buf) == count - 1);
		int idx = fbdict_lookup(buf, &key, 0);
		test_assert(idx >= 0);
		test_assert(test_key_be64(buf, idx) != key);
	}

	free(tests);
	free(buf);
}

int main(int argc, char *argv[])
{
	test_init(argc, argv);

	int blocksize = 1 << 12;

	if (test_start("test01"))
		test01(blocksize);
	test_end();

	return test_failures();
}
