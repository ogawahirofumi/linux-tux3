#ifndef _TEST_H
#define _TEST_H

#include <time.h>
#include <sys/types.h>

#define test_assert(x)	({						\
	int __test_res = !(x);						\
	if (__test_res) {						\
		printf("[%s:%s] %s:%d:%s: assertion failed: %s\n",	\
		       test_series(), test_name(), __FILE__, __LINE__,	\
		       __func__, #x);					\
		test_assert_failed();					\
	}								\
	__test_res;							\
})

#ifndef NSEC_PER_SEC
#define NSEC_PER_SEC	1000000000L
#endif

typedef long long		test_time_t;

struct test_elapse {
	test_time_t start;
};

static test_time_t test_ns_gettime(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (test_time_t)ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

static inline void test_elapse_start(struct test_elapse *e)
{
	e->start = test_ns_gettime();
}

static inline test_time_t test_elapse_stop(struct test_elapse *e)
{
	return test_ns_gettime() - e->start;
}

int test_init(int argc, char *argv[]);
const char *test_series(void);
const char *test_name(void);
void test_assert_failed(void);
int test_start(const char *name);
void test_end(void);
int test_failures(void);
void *test_alloc_shm(size_t size);
void test_free_shm(void *ptr, size_t size);

#endif /* !_TEST_H */
