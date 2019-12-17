/*
 * micro benchmark helpers
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <errno.h>
#include <error.h>
#include <unistd.h>

#include <string.h>
#include <time.h>
#include <math.h>

#include "test.h"
#include "bench.h"

static bool opt_detail_default = false;

void bench_set_detail_default(bool v)
{
	opt_detail_default = v;
}

#ifndef min
#define min(a, b)	((a) > (b) ? (b) : (a))
#endif
#ifndef max
#define max(a, b)	((a) < (b) ? (b) : (a))
#endif

#ifndef NSEC_PER_SEC
#define NSEC_PER_SEC	((b_uint64)1000000000)
#endif

static b_int64 bench_get_nsecs(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (b_int64)ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

static void bench_test_nop(void *_arg, int i)
{
	(void)_arg;
	(void)i;
}

static struct bench bench_nop = {
	.name = "bench:nop",
	.test = bench_test_nop,
};

static int uint64_cmp(const void *_a, const void *_b)
{
	const b_int64 *a = _a, *b = _b;
	if (*a < *b)
		return -1;
	if (*a > *b)
		return 1;
	return 0;
}

static void bench_diff_rusage(struct rusage *res, struct rusage *s,
			      struct rusage *e)
{
	struct timeval t;
	timersub(&e->ru_utime, &s->ru_utime, &t);
	timeradd(&res->ru_utime, &t, &res->ru_utime);
	timersub(&e->ru_stime, &s->ru_stime, &t);
	timeradd(&res->ru_stime, &t, &res->ru_stime);
#define DIFF(f)		res->f += e->f - s->f
	DIFF(ru_maxrss);
	DIFF(ru_ixrss);
	DIFF(ru_idrss);
	DIFF(ru_isrss);
	DIFF(ru_minflt);
	DIFF(ru_majflt);
	DIFF(ru_nswap);
	DIFF(ru_inblock);
	DIFF(ru_oublock);
	DIFF(ru_msgsnd);
	DIFF(ru_msgrcv);
	DIFF(ru_nsignals);
	DIFF(ru_nvcsw);
	DIFF(ru_nivcsw);
#undef DIFF
}

static void bench_calc_res(struct bench_result *res)
{
	b_int64 e_total = 0;

	if (res->nr == 0)
		return;

	int nr = res->detail ? res->nr : 1;
	size_t size = sizeof(*res->elapse) * nr;
	b_int64 *tmp = malloc(size);
	if (!tmp)
		error(1, errno, "malloc");
	memcpy(tmp, res->elapse, size);
	qsort(tmp, nr, sizeof(*tmp), uint64_cmp);

	for (int i = 0; i < nr; i++)
		e_total += tmp[i];
	res->total = e_total;
	res->min = tmp[0];
	res->max = tmp[nr - 1];
	res->median = tmp[nr / 2];
	res->mean = (double)e_total / res->nr;

	double e_var = 0;
	for (int i = 0; i < nr; i++) {
		double diff = (double)tmp[i] - res->mean;
		e_var += diff * diff;
	}
	res->stddev = sqrt(e_var / nr);
	res->sstddev = sqrt(e_var / (nr > 1 ? nr - 1 : nr));
	res->stderr = res->stddev / sqrt(nr);

	free(tmp);
}

void bench_res_free(struct bench_result *res)
{
	if (res->elapse)
		free(res->elapse);
}

void bench_loop(struct bench_result *res, bool detail,
		struct bench *bench, int nr, void *_arg)
{
	memset(res, 0, sizeof(*res));
	res->nr = nr;
	res->detail = detail;

	int nr_elapse = detail ? nr : 1;
	b_int64 *elapse = calloc(nr_elapse, sizeof(*elapse));
	if (!elapse)
		error(1, errno, "malloc");
	res->elapse = elapse;

	struct rusage susage, eusage;
	getrusage(RUSAGE_SELF, &susage);
	for (int i = 0; i < nr; i++) {
		if (bench->pre)
			bench->pre(_arg, i);

		b_int64 s = bench_get_nsecs();
		bench->test(_arg, i);
		b_int64 e = bench_get_nsecs();
		if (detail)
			elapse[i] = e - s;
		else
			elapse[0] += e - s;

		if (bench->post)
			bench->post(_arg, i);
	}
	getrusage(RUSAGE_SELF, &eusage);

	bench_diff_rusage(&res->rusage, &susage, &eusage);
	bench_calc_res(res);
}

void bench_res_print(struct bench_result *res, FILE *fp_out, FILE *fp_log)
{
	if (!res->detail) {
		fprintf(fp_out,
			"elapse: total %llu ns (loop %d times)\n"
			"        avg %.2f ns\n",
			res->total, res->nr, res->mean);
	} else {
		fprintf(fp_out,
			"elapse: total %llu ns (loop %d times)\n"
			"        median %llu ns, min %llu ns, max %llu ns\n"
			"        avg %.2f ns, sstddev %.2f, stddev %.2f, stderr %.2f\n",
			res->total, res->nr,
			res->median, res->min, res->max,
			res->mean, res->sstddev, res->stddev, res->stderr);
	}
	fprintf(fp_out,
		"rusage: utime %ld.%06ld s, stime %ld.%06ld s\n"
		"        minflt %ld, majflt %ld, inblock %ld, oublock %ld\n"
		"        nvcsw %ld, nivcsw %ld\n",
		res->rusage.ru_utime.tv_sec, res->rusage.ru_utime.tv_usec,
		res->rusage.ru_stime.tv_sec, res->rusage.ru_stime.tv_usec,
		res->rusage.ru_minflt, res->rusage.ru_majflt,
		res->rusage.ru_inblock, res->rusage.ru_oublock,
		res->rusage.ru_nvcsw, res->rusage.ru_nivcsw);

	if (!fp_log)
		return;

	for (int i = 0; i < res->nr; i++)
		fprintf(fp_log, "%llu\n", res->elapse[i]);
}

void bench_run(struct bench *bench, int nr, void *_arg)
{
	struct bench_result res;
	bench_loop(&res, opt_detail_default, bench, nr, _arg);
	if (test_series())
		printf("[%s:%s]\n", test_series(), bench->name);
	else
		printf("[%s]\n", bench->name);
	bench_res_print(&res, stdout, NULL);
	bench_res_free(&res);
}

void bench_measure_overhead(void)
{
	struct bench_result res;
	/* warmup */
	bench_loop(&res, true, &bench_nop, 5000, NULL);
	/* measure nop overhead */
	bench_loop(&res, false, &bench_nop, 5000, NULL);
	printf("#\n"
	       "# bench overhead (nop):\n"
	       "#     avg %.2f ns, sstddev %.2f ns\n"
	       "#     minflt %ld, majflt %ld, nvcsw %ld, nivcsw %ld\n"
	       "#\n",
	       res.mean, res.sstddev,
	       res.rusage.ru_minflt, res.rusage.ru_majflt,
	       res.rusage.ru_nvcsw, res.rusage.ru_nivcsw);
}
