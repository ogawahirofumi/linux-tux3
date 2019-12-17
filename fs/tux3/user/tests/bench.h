#ifndef _BENCH_H
#define _BENCH_H

#include <stdint.h>
#include <sys/time.h>
#include <sys/resource.h>

/*
 * uint64_t can be "unsigned long" or "unsigned long long", so not
 * printf friendly.
 */
typedef unsigned long long	b_int64;

struct bench {
	const char *name;
	void (*pre)(void *_arg, int i);
	void (*test)(void *_arg, int i);
	void (*post)(void *_arg, int i);
};

struct bench_result {
	bool detail;		/* if detail, measures for each test */
	int nr;			/* number of test */
	b_int64 *elapse;	/* elapse per test (need detail) */
	b_int64 total;		/* total of elapse */
	b_int64 min;		/* minimum elapse (need detail) */
	b_int64 max;		/* maximum elapse (need detail) */
	b_int64 median;		/* median of elapses (need detail) */
	double mean;		/* mean of elapses */
	double stddev;		/* standard deviation (need detail) */
	double sstddev;		/* sample stddev (n - 1) (need detail) */
	double stderr;		/* standard error of mean (need detail) */
	struct rusage rusage;
};

void bench_set_detail_default(bool v);
void bench_res_free(struct bench_result *res);
void bench_loop(struct bench_result *res, bool no_detail,
		struct bench *bench, int nr, void *_arg);
void bench_res_print(struct bench_result *res, FILE *fp_out, FILE *fp_log);
void bench_run(struct bench *bench, int nr, void *_arg);
void bench_measure_overhead(void);

#endif /* !_BENCH_H */
