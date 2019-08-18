/*
 * libklib tests
 */

#include "tux3user.h"
#include "test.h"

static void test_atomic(void *_arg)
{
	atomic_t v1 = ATOMIC_LONG_INIT(0);
#define VAL		(1U << 31)
	test_assert(atomic_fetch_or(VAL, &v1) == 0);
	test_assert(atomic_read(&v1) == VAL);
#undef VAL

#define VAL		(1ULL << 63)
	atomic64_t v2 = ATOMIC64_INIT(0);
	test_assert(atomic64_fetch_or(VAL, &v2) == 0);
	test_assert(atomic64_read(&v2) == VAL);
#undef VAL

#define VAL		(1UL << 31)
	atomic_long_t v3 = ATOMIC_LONG_INIT(0);
	test_assert(atomic_long_fetch_or(VAL, &v3) == 0);
	test_assert(atomic_long_read(&v3) == VAL);
#undef VAL
}
TEST_DEFINE(TEST_UNIT, "atomic", test_atomic);

static void test_rw_once(void *_arg)
{
	u8 v1 = 0;
	u16 v2 = 0;
	u32 v4 = 0;
	u64 v8 = 0;
#ifdef __SIZEOF_INT128__
	/* test for internally uses memcpy() that can not be atomic. */
	__int128 v16 = 0;
#endif

	WRITE_ONCE(v1, 10);
	WRITE_ONCE(v2, 10);
	WRITE_ONCE(v4, 10);
	WRITE_ONCE(v8, 10);
#ifdef __SIZEOF_INT128__
	WRITE_ONCE(v16, 10);
#endif
	test_assert(READ_ONCE(v1) == 10);
	test_assert(READ_ONCE(v2) == 10);
	test_assert(READ_ONCE(v4) == 10);
	test_assert(READ_ONCE(v8) == 10);
#ifdef __SIZEOF_INT128__
	test_assert(READ_ONCE(v16) == 10);
#endif
}
TEST_DEFINE(TEST_UNIT, "rw_once", test_rw_once);

static void test_err(void *_arg)
{
	long err = -EINVAL;
	void *err_ptr = ERR_PTR(err);

	test_assert(PTR_ERR(err_ptr) == err);
	test_assert(IS_ERR_VALUE(err) == true);
	test_assert(IS_ERR_OR_NULL(err_ptr) == true);
	test_assert(IS_ERR_OR_NULL(NULL) == true);
	test_assert(ERR_CAST(err_ptr) == err_ptr);
	test_assert(PTR_ERR_OR_ZERO(err_ptr) == err);
	test_assert(PTR_ERR_OR_ZERO(0) == 0);
}
TEST_DEFINE(TEST_UNIT, "err", test_err);

static void test_find_bit(void *_arg)
{
	unsigned long zero[2];
	unsigned long full[2];
	unsigned long nbits = (sizeof(zero) * 8) - 2;

	memset(zero, 0x00, sizeof(zero));
	memset(full, 0xff, sizeof(full));

	test_assert(find_next_bit(zero, nbits, 1) == nbits);
	test_assert(find_next_bit(full, nbits, 1) == 1);
	test_assert(find_next_and_bit(zero, full, nbits, 1) == nbits);
	test_assert(find_next_and_bit(full, full, nbits, 1) == 1);
	test_assert(find_first_bit(zero, nbits) == nbits);
	test_assert(find_first_bit(full, nbits) == 0);
	test_assert(find_last_bit(zero, nbits) == nbits);
	test_assert(find_last_bit(full, nbits) == nbits - 1);
	test_assert(find_next_zero_bit(full, nbits, 1) == nbits);
	test_assert(find_next_zero_bit(zero, nbits, 1) == 1);
	test_assert(find_first_zero_bit(full, nbits) == nbits);
	test_assert(find_first_zero_bit(zero, nbits) == 0);
}
TEST_DEFINE(TEST_UNIT, "find_bit", test_find_bit);

/* dummpy function for testing */
void truncate_buffers_range(map_t *map, loff_t lstart, loff_t lend)
{
}

static void test_fs(void *_arg)
{
	struct inode inode = { .i_size = 0,};
	truncate_setsize(&inode, 0);
	truncate_setsize(&inode, 1);
	truncate_setsize(&inode, 0);

	/* timespec64_trunc tests */
	struct timespec64 t, ts = { .tv_sec = 10, .tv_nsec = 999999999, };

	t = timespec64_trunc(ts, 1);
	test_assert(t.tv_sec == ts.tv_sec && t.tv_nsec == ts.tv_nsec);
	t = timespec64_trunc(ts, NSEC_PER_SEC);
	test_assert(t.tv_sec == ts.tv_sec && t.tv_nsec == 0);
	t = timespec64_trunc(ts, 1000);
	test_assert(t.tv_sec == ts.tv_sec && t.tv_nsec == 999999000);
#if 0
	/* the case of WARN() */
	t = timespec64_trunc(ts, 2 * NSEC_PER_SEC);
	test_assert(t.tv_sec == ts.tv_sec && t.tv_nsec == ts.tv_nsec);
#endif
}
TEST_DEFINE(TEST_UNIT, "fs", test_fs);

static void test_lock(void *_arg)
{
	static DEFINE_SPINLOCK(spin);
	static DECLARE_RWSEM(rwsem);
	static DEFINE_MUTEX(mutex);

	spin_lock_init(&spin);
	spin_lock(&spin);
	spin_unlock(&spin);
	spin_trylock(&spin);
	spin_unlock(&spin);
	test_assert(!spin_is_locked(&spin));
	atomic_t v = ATOMIC_INIT(1);
	test_assert(atomic_dec_and_lock(&v, &spin));

	init_rwsem(&rwsem);
	down_read(&rwsem);
	up_read(&rwsem);
	down_read_trylock(&rwsem);
	up_read(&rwsem);
	down_write(&rwsem);
	up_write(&rwsem);
	down_write_trylock(&rwsem);
	up_write(&rwsem);
	test_assert(!rwsem_is_locked(&rwsem));

	mutex_init(&mutex);
	mutex_lock(&mutex);
	mutex_unlock(&mutex);
	mutex_trylock(&mutex);
	mutex_unlock(&mutex);
	test_assert(!mutex_is_locked(&mutex));
}
TEST_DEFINE(TEST_UNIT, "lock", test_lock);

static void test_mm(void *_arg)
{
	char *s = "1234", *p;

	test_assert(kstrdup(NULL, GFP_KERNEL) == NULL);
	p = kstrdup(s, GFP_KERNEL);
	test_assert(p != NULL && p != s && !strcmp(s, p));
	kfree(p);
	test_assert(kstrndup(NULL, 2, GFP_KERNEL) == NULL);
	p = kstrndup(s, 2, GFP_KERNEL);
	test_assert(p != NULL && p != s && !strcmp("12", p));
	kfree(p);
	p = kmemdup(s, 4, GFP_KERNEL);
	test_assert(p != NULL && p != s && !memcmp(s, p, 4));
	kfree(p);
	test_assert(kmemdup_nul(NULL, 2, GFP_KERNEL) == NULL);
	p = kmemdup_nul(s, 2, GFP_KERNEL);
	test_assert(p != NULL && p != s && !memcmp("12", p, 2));
	kfree(p);
}
TEST_DEFINE(TEST_UNIT, "mm", test_mm);

static void test_parser(void *_arg)
{
	enum {
		Opt_1, Opt_2, Opt_3, Opt_4, Opt_5, Opt_6, Opt_7, Opt_8,
		Opt_err,
	};
	static const match_table_t tokens = {
		{Opt_2, "a=%10s"},
		{Opt_3, "b=%d"},
		{Opt_4, "c=%u"},
		{Opt_5, "d=%o"},
		{Opt_6, "e=%x"},
		{Opt_7, "f=%%"},
		{Opt_8, "g=%e"},	/* invalid specifier: ignored */
		{Opt_err, NULL},
	};
	substring_t args[MAX_OPT_ARGS];
	char *s;
	int v;

	/* not match */
	test_assert(match_token("h=b", tokens, args) == Opt_err);

	test_assert(match_token("a=testtest", tokens, args) == Opt_2);
	s = match_strdup(&args[0]);
	test_assert(!strcmp(s, "testtest"));
	test_assert(match_wildcard("tes", s) == false);
	test_assert(match_wildcard("tes*?te*", s) == true);
	test_assert(match_wildcard("tes?*st", s) == true);
	test_assert(match_wildcard("testtest*", s) == true);
	match_strlcpy(s, &args[0], 5);
	test_assert(!strcmp(s, "test"));
	kfree(s);

	test_assert(match_token("b=10", tokens, args) == Opt_3);
	test_assert(!match_int(&args[0], &v));
	test_assert(v == 10);
	test_assert(match_token("c=10", tokens, args) == Opt_4);
	test_assert(!match_int(&args[0], &v));
	test_assert(v == 10);
	test_assert(match_token("d=10", tokens, args) == Opt_5);
	test_assert(!match_octal(&args[0], &v));
	test_assert(v == 010);
	test_assert(match_token("e=10", tokens, args) == Opt_6);
	test_assert(!match_hex(&args[0], &v));
	test_assert(v == 0x10);

	test_assert(match_token("f=a", tokens, args) == Opt_err);
	test_assert(match_token("f=%", tokens, args) == Opt_7);

	test_assert(match_token("g=10", tokens, args) == Opt_err);
}
TEST_DEFINE(TEST_UNIT, "parser", test_parser);

static void test_refcount(void *_arg)
{
	refcount_t r = REFCOUNT_INIT(0);

	test_assert(refcount_add_not_zero_checked(1, &r) == false);

	refcount_set(&r, 1);
	test_assert(refcount_add_not_zero_checked(1, &r) == true);
	refcount_add_checked(1, &r);
	test_assert(refcount_inc_not_zero_checked(&r) == true);
	refcount_inc_checked(&r);
	test_assert(refcount_sub_and_test_checked(1, &r) == false);
	test_assert(refcount_dec_and_test_checked(&r) == false);
	refcount_dec_checked(&r);

	refcount_set(&r, 1);
	test_assert(refcount_dec_if_one(&r) == true);
	refcount_set(&r, 2);
	test_assert(refcount_dec_not_one(&r) == true);

	DEFINE_MUTEX(mutex);
	refcount_set(&r, 1);
	test_assert(refcount_dec_and_mutex_lock(&r, &mutex) == true);
	mutex_unlock(&mutex);
	refcount_set(&r, 2);
	test_assert(refcount_dec_and_mutex_lock(&r, &mutex) == false);

	DEFINE_SPINLOCK(lock);
	refcount_set(&r, 1);
	test_assert(refcount_dec_and_lock(&r, &lock) == true);
	spin_unlock(&lock);
	refcount_set(&r, 2);
	test_assert(refcount_dec_and_lock(&r, &lock) == false);
	unsigned long flags;
	refcount_set(&r, 1);
	test_assert(refcount_dec_and_lock_irqsave(&r, &lock, &flags) == true);
	spin_unlock(&lock);
	refcount_set(&r, 2);
	test_assert(refcount_dec_and_lock_irqsave(&r, &lock, &flags) == false);
}
TEST_DEFINE(TEST_UNIT, "refcount", test_refcount);

#include <libklib/seq_file.h>
static void test_seqfile(void *_arg)
{
	char buf[4096];
	struct seq_file seq = {
		.buf = buf,
		.size = sizeof(buf),
	};

	seq_printf(&seq, "test %s\n", "seq");
	test_assert(seq_has_overflowed(&seq) == false);
	seq_putc(&seq, '#');
	test_assert(seq_has_overflowed(&seq) == false);
	seq_puts(&seq, "test2\n");
	test_assert(seq_has_overflowed(&seq) == false);

	test_assert(!memcmp("test seq\n#test2\n", seq.buf, seq.count));
}
TEST_DEFINE(TEST_UNIT, "seqfile", test_seqfile);

static void test_slab(void *_arg)
{
	struct kmem_cache *cachep;
	void *p;

	p = kmalloc(10, GFP_KERNEL);
	test_assert(p);
	kfree(p);
	p = kzalloc(16, GFP_KERNEL);
	test_assert(p);
	test_assert(find_next_bit(p, 16 * 8, 0) == 16 * 8);
	kfree(p);

	cachep = kmem_cache_create_usercopy("test", 10, 0, 0, 0, 10, NULL);
	test_assert(cachep);
	kmem_cache_destroy(cachep);

#define TEST_SIZE	64
#define TEST_ALIGN	32
	cachep = kmem_cache_create("test", TEST_SIZE, TEST_ALIGN, 0, NULL);
	test_assert(cachep);
	p = kmem_cache_zalloc(cachep, GFP_KERNEL);
	test_assert(((unsigned long)p & (TEST_ALIGN - 1)) == 0);
	test_assert(find_next_bit(p, TEST_SIZE * 8, 0) == TEST_SIZE * 8);
	kmem_cache_free(cachep, p);
	kmem_cache_destroy(cachep);
#undef SIZE
}
TEST_DEFINE(TEST_UNIT, "slab", test_slab);

static void test_time(void *_arg)
{
	struct timespec t;
	t = ns_to_timespec(0);
	test_assert(t.tv_sec == 0 && t.tv_nsec == 0);
	t = ns_to_timespec(NSEC_PER_SEC * 2 + 100);
	test_assert(t.tv_sec == 2 && t.tv_nsec == 100);
	t = ns_to_timespec(-100);
	test_assert(t.tv_sec == -1 && t.tv_nsec == 999999900);

	struct timeval tv;
	tv = ns_to_timeval(NSEC_PER_SEC * 2 + 100);
	test_assert(tv.tv_sec == 2 && tv.tv_usec == 0);

	struct timespec64 t64;
	t64 = ns_to_timespec64(0);
	test_assert(t64.tv_sec == 0 && t64.tv_nsec == 0);
	t64 = ns_to_timespec64(NSEC_PER_SEC * 2 + 100);
	test_assert(t64.tv_sec == 2 && t64.tv_nsec == 100);
	t64 = ns_to_timespec64(-100);
	test_assert(t64.tv_sec == -1 && t64.tv_nsec == 999999900);

	struct timespec64 r, a, b;
	a = (struct timespec64){ 1, 500000000 };
	b = (struct timespec64){ 2, 500000001 };
	r = timespec64_add(a, b);
	test_assert(r.tv_sec == 4 && r.tv_nsec == 1);
	r = timespec64_add_safe(a, b);
	test_assert(r.tv_sec == 4 && r.tv_nsec == 1);
	a = (struct timespec64){ 1, 500000000 };
	b = (struct timespec64){ 2, 499999999 };
	r = timespec64_sub(b, a);
	test_assert(r.tv_sec == 0 && r.tv_nsec == 999999999);
	timespec64_add_ns(&a, 500000001);
	test_assert(a.tv_sec == 2 && a.tv_nsec == 1);

	a = (struct timespec64){ LLONG_MAX, 500000000 };
	b = (struct timespec64){ 0, 500000001 };
	r = timespec64_add_safe(a, b);
	test_assert(r.tv_sec == TIME64_MAX && r.tv_nsec == 0);

	a = (struct timespec64){ 1, 1 };
	test_assert(timespec64_valid(&a) == true);
	test_assert(timespec64_valid_strict(&a) == true);
	test_assert(timespec64_valid_settod(&a) == true);
	a = (struct timespec64){ -1, 0 };
	test_assert(timespec64_valid(&a) == false);
	test_assert(timespec64_valid_strict(&a) == false);
	test_assert(timespec64_valid_settod(&a) == false);
	a = (struct timespec64){ 0, 1000000001 };
	test_assert(timespec64_valid(&a) == false);
	test_assert(timespec64_valid_strict(&a) == false);
	test_assert(timespec64_valid_settod(&a) == false);
	a = (struct timespec64){ LLONG_MAX, 0 };
	test_assert(timespec64_valid(&a) == true);
	test_assert(timespec64_valid_strict(&a) == false);
	test_assert(timespec64_valid_settod(&a) == false);
}
TEST_DEFINE(TEST_UNIT, "time", test_time);

static void test_fls(void *_arg)
{
	test_assert(__fls(1) == 0);
	test_assert(fls64(0) == 0);
	test_assert(fls64(1) == 1);
}
TEST_DEFINE(TEST_UNIT, "fls", test_fls);

static void test_bitops(void *_arg)
{
	unsigned long v[2] = {};

	set_bit(BITS_PER_LONG + 1, v);
	test_assert(test_bit(BITS_PER_LONG + 1, v) != 0);
	change_bit(BITS_PER_LONG + 1, v);
	test_assert(test_bit(BITS_PER_LONG + 1, v) == 0);
	test_assert(test_and_set_bit(BITS_PER_LONG + 1, v) == 0);
	test_assert(test_and_set_bit(BITS_PER_LONG + 1, v) != 0);
	test_assert(test_and_clear_bit(BITS_PER_LONG + 1, v) != 0);
	test_assert(test_and_clear_bit(BITS_PER_LONG + 1, v) == 0);
	test_assert(test_and_change_bit(BITS_PER_LONG + 1, v) == 0);
	test_assert(test_and_change_bit(BITS_PER_LONG + 1, v) != 0);
}
TEST_DEFINE(TEST_UNIT, "bitops", test_bitops);

int main(int argc, char *argv[])
{
	test_init(argc, argv);

	test_run(NULL);

	return test_failures();
}
