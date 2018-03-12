#ifndef LIBKLIB_TIME64_H
#define LIBKLIB_TIME64_H

#include <libklib/bitops.h>

typedef __s64 time64_t;
typedef __u64 timeu64_t;

#if BITS_PER_LONG == 64
/* this trick allows us to optimize out timespec64_to_timespec */
# define timespec64 timespec
#define itimerspec64 itimerspec
#else
struct timespec64 {
	time64_t	tv_sec;			/* seconds */
	long		tv_nsec;		/* nanoseconds */
};
#endif

/* Parameters used to convert the timespec values: */
#define MSEC_PER_SEC	1000L
#define USEC_PER_MSEC	1000L
#define NSEC_PER_USEC	1000L
#define NSEC_PER_MSEC	1000000L
#define USEC_PER_SEC	1000000L
#define NSEC_PER_SEC	1000000000L
#define FSEC_PER_SEC	1000000000000000LL

static inline int timespec64_equal(const struct timespec64 *a,
				   const struct timespec64 *b)
{
	return (a->tv_sec == b->tv_sec) && (a->tv_nsec == b->tv_nsec);
}

/*
 * lhs < rhs:  return <0
 * lhs == rhs: return 0
 * lhs > rhs:  return >0
 */
static inline int timespec64_compare(const struct timespec64 *lhs, const struct timespec64 *rhs)
{
	if (lhs->tv_sec < rhs->tv_sec)
		return -1;
	if (lhs->tv_sec > rhs->tv_sec)
		return 1;
	return lhs->tv_nsec - rhs->tv_nsec;
}

/**
 * timespec64_to_ns - Convert timespec64 to nanoseconds
 * @ts:		pointer to the timespec64 variable to be converted
 *
 * Returns the scalar nanosecond representation of the timespec64
 * parameter.
 */
static inline s64 timespec64_to_ns(const struct timespec64 *ts)
{
	return ((s64) ts->tv_sec * NSEC_PER_SEC) + ts->tv_nsec;
}

/**
 * ns_to_timespec64 - Convert nanoseconds to timespec64
 * @nsec:	the nanoseconds value to be converted
 *
 * Returns the timespec64 representation of the nsec parameter.
 */
extern struct timespec64 ns_to_timespec64(const s64 nsec);

#endif /* !LIBKLIB_TIME64_H */
