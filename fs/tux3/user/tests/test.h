#ifndef _TEST_H
#define _TEST_H

#include <sys/time.h>
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

struct test_elapse {
	struct timeval start;
};

static inline void test_elapse_start(struct test_elapse *e)
{
	gettimeofday(&e->start, NULL);
}

static inline struct timeval test_elapse_stop(struct test_elapse *e)
{
	struct timeval end, diff;
	gettimeofday(&end, NULL);
	timersub(&end, &e->start, &diff);
	return diff;
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
