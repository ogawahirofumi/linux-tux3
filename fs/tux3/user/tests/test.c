#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <error.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <unistd.h>
#include <assert.h>
#include <fnmatch.h>

#include "test.h"
#include "bench.h"
#include "../aligncheck.h"

/* dummy define to guarantee define at least one TEST_DEFINE(). */
static void test_dummy(void *_arg)
{
	(void)_arg;
}
TEST_DEFINE(TEST_UNIT, "test_dummy", test_dummy);

#define skip_test_dummy(def)				\
	(def == &TEST_DEFINE_NAME(test_dummy) ? def++ : def)
#define for_each_tests(def)						\
	for (def = &__start_test_define[0], skip_test_dummy(def);	\
	     def < __stop_test_define;					\
	     def++, skip_test_dummy(def))

#define TEST_NEST_MAX		10
#define TEST_FILTER_MAX		10

struct test_env {
	const char *series;
	int test_fail_count;

	struct {
		const char *name;
		pid_t child;
		struct test_elapse elapse;
		int fail_cnt;
	} test[TEST_NEST_MAX];
	int nest;
	int forked;
	bool filtered;
};

static struct test_env test_env;
struct filter {
	const char *inc[TEST_FILTER_MAX];
	int nr_inc;
	const char *exc[TEST_FILTER_MAX];
	int nr_exc;
};
static struct filter opt_filter;
static enum test_type opt_test_type = TEST_UNIT;

/*
 * - If test matched to "include" filter, return false even if included
 *   in "exclude" filter.
 * - If test matched to "exclude" filter, return true.
 * - If not matched, return false.
 */
static bool test_is_filtered(const char *test)
{
	int i;

	for (i = 0; i < opt_filter.nr_inc; i++) {
		if (fnmatch(opt_filter.inc[i], test, 0) == 0)
			return false;
	}
	for (i = 0; i < opt_filter.nr_exc; i++) {
		if (fnmatch(opt_filter.exc[i], test, 0) == 0)
			return true;
	}

	return false;
}

static void test_usage(const char *progname)
{
	printf("Usage: %s [options] <args>\n"
	       "\n"
	       "  -i <pattern>     Including test name (glob pattern)\n"
	       "  -x <pattern>     Excluding test name (glob pattern)\n"
	       "  -u               Run unit test\n"
	       "  -b               Run bench test\n"
	       "  -d               Show detailed result of bench\n"
	       "  -l               List all tests\n"
	       "  -h               Print this help\n",
	       progname);
}

static void test_list_test_defines(void)
{
	struct test_define *def;
	const char *name[] = {
		[TEST_UNIT]	= "unit",
		[TEST_BENCH]	= "bench",
	};

	for_each_tests(def)
		printf("[%s] \"%s\"\n", name[def->type], def->name);
}

int test_init(int argc, char *argv[])
{
	const char *argv0 = argv[0];
	const char *progname;
	int opt;

	progname = strrchr(argv0, '/');
	if (progname == NULL)
		progname = argv0;
	else
		progname++;

	while ((opt = getopt(argc, argv, "i:x:ubdlh")) != -1) {
		switch (opt) {
		case 'i':
			if (opt_filter.nr_inc >= TEST_FILTER_MAX)
				error(1, 0, "too many include filters");

			opt_filter.inc[opt_filter.nr_inc] = optarg;
			opt_filter.nr_inc++;
			break;
		case 'x':
			if (opt_filter.nr_exc >= TEST_FILTER_MAX)
				error(1, 0, "too many exclude filters");

			opt_filter.exc[opt_filter.nr_exc] = optarg;
			opt_filter.nr_exc++;
			break;
		case 'u':
			opt_test_type = TEST_UNIT;
			break;
		case 'b':
			opt_test_type = TEST_BENCH;
			break;
		case 'd':
			bench_set_detail_default(true);
			break;
		case 'l':
			test_list_test_defines();
			exit(0);
		case 'h':
		default: /* '?' */
			test_usage(progname);
			exit(EXIT_FAILURE);
		}
	}
	/* If specified only include filters, add the default exclude filter. */
	if (opt_filter.nr_inc && !opt_filter.nr_exc) {
		opt_filter.exc[opt_filter.nr_exc] = "*";
		opt_filter.nr_exc++;
	}

	test_env.series = progname;
	test_env.test_fail_count = 0;
	test_env.nest = -1;
	test_env.forked = 0;
	test_env.filtered = false;

	/* Make sure aligncheck is initialized */
	init_alignment_check();

	return optind;
}

enum test_type test_type(void)
{
	return opt_test_type;
}

const char *test_series(void)
{
	return test_env.series;
}

const char *test_name(void)
{
	return test_env.test[test_env.nest].name;
}

void test_assert_failed(void)
{
	test_env.test[test_env.nest].fail_cnt++;
}

int test_start(const char *test)
{
	/* Need to run test? */
	if (test_is_filtered(test)) {
		test_env.filtered = true;
		return 0;
	}

	int nest = ++test_env.nest;
	assert(nest < TEST_NEST_MAX);

	test_env.test[nest].name = test;

	/* Flush buffering data to avoid duplicate by forking */
	fflush(NULL);

	test_env.test[nest].child = fork();
	assert(test_env.test[nest].child >= 0);
	if (test_env.test[nest].child == 0) {
		test_env.test[nest].fail_cnt = 0;
		test_elapse_start(&test_env.test[nest].elapse);
		return 1;
	}

	test_env.forked = 1;
	return 0;
}

void test_end(void)
{
	/* Test is filtered? */
	if (test_env.filtered) {
		test_env.filtered = false;
		return;
	}

	int nest = test_env.nest;
	int test_fail_count = test_env.test_fail_count;
	pid_t err;
	int status;

	/* Dead as child? */
	if (!test_env.forked) {
		test_time_t diff;
		diff = test_elapse_stop(&test_env.test[nest].elapse);

		if (opt_test_type == TEST_UNIT) {
			printf("[%s:%s] time %3lld.%09lld secs\n",
			       test_env.series, test_env.test[nest].name,
			       diff / NSEC_PER_SEC, diff % NSEC_PER_SEC);
		}

		exit(!!test_env.test[nest].fail_cnt);
	}

	test_env.forked = 0;
	err = waitpid(test_env.test[nest].child, &status, 0);
	assert(err >= 0);

	if (WIFEXITED(status)) {
		if (opt_test_type == TEST_UNIT) {
			printf("[%s:%s] %s\n",
			       test_env.series, test_env.test[nest].name,
			       WEXITSTATUS(status) ? "FAILED" : "OK");
		}

		if (WEXITSTATUS(status))
			test_env.test_fail_count++;
	} else if (WIFSIGNALED(status)) {
		printf("[%s:%s] FAILED by sig (%d)%s\n",
		       test_env.series, test_env.test[nest].name,
		       WTERMSIG(status), WCOREDUMP(status) ? " coredump" : "");

		if (WCOREDUMP(status)) {
			char corename[4096];
			snprintf(corename, sizeof(corename), "%s.%s.core",
				 test_env.series, test_env.test[nest].name);
			rename("core", corename);
		}

		test_env.test_fail_count++;
	}

	test_env.nest--;
	/* If nexted test failed, propagate failure to parent */
	if (test_env.test_fail_count > test_fail_count)
		test_assert_failed();
}

int test_failures(void)
{
	return test_env.test_fail_count;
}

void test_run(void *_arg)
{
	struct test_define *def;

	/* FIXME: run tests parallel? */
	for_each_tests(def) {
		if (def->type != opt_test_type)
			continue;

		if (test_start(def->name))
			def->test(_arg);
		test_end();
	}
}

/*
 * Utility functions for test
 */

/* Create shared memory to share with child process */
void *test_alloc_shm(size_t size)
{
	void *ptr;

	ptr = mmap(NULL, size, PROT_READ | PROT_WRITE,
		   MAP_ANONYMOUS | MAP_SHARED, -1, 0);
	assert(ptr != MAP_FAILED);

	return ptr;
}

void test_free_shm(void *ptr, size_t size)
{
	munmap(ptr, size);
}
