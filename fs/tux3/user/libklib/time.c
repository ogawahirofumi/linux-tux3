#include <tux3user.h>
#include <libklib/time.h>
#include <libklib/math64.h>

/**
 * timespec_trunc - Truncate timespec to a granularity
 * @t: Timespec
 * @gran: Granularity in ns.
 *
 * Truncate a timespec to a granularity. Always rounds down. gran must
 * not be 0 nor greater than a second (NSEC_PER_SEC, or 10^9 ns).
 */
struct timespec timespec_trunc(struct timespec t, unsigned gran)
{
	/* Avoid division in the common cases 1 ns and 1 s. */
	if (gran == 1) {
		/* nothing */
	} else if (gran == NSEC_PER_SEC) {
		t.tv_nsec = 0;
	} else if (gran > 1 && gran < NSEC_PER_SEC) {
		t.tv_nsec -= t.tv_nsec % gran;
	} else {
		WARN(1, "illegal file time granularity: %u", gran);
	}
	return t;
}

#if BITS_PER_LONG == 32
/**
 * ns_to_timespec - Convert nanoseconds to timespec
 * @nsec:       the nanoseconds value to be converted
 *
 * Returns the timespec representation of the nsec parameter.
 */
struct timespec ns_to_timespec(const s64 nsec)
{
	struct timespec ts;
	s32 rem;

	if (!nsec)
		return (struct timespec) {0, 0};

	ts.tv_sec = div_s64_rem(nsec, NSEC_PER_SEC, &rem);
	if (unlikely(rem < 0)) {
		ts.tv_sec--;
		rem += NSEC_PER_SEC;
	}
	ts.tv_nsec = rem;

	return ts;
}
#endif

/**
 * ns_to_timeval - Convert nanoseconds to timeval
 * @nsec:       the nanoseconds value to be converted
 *
 * Returns the timeval representation of the nsec parameter.
 */
struct timeval ns_to_timeval(const s64 nsec)
{
	struct timespec ts = ns_to_timespec(nsec);
	struct timeval tv;

	tv.tv_sec = ts.tv_sec;
	tv.tv_usec = (suseconds_t) ts.tv_nsec / 1000;

	return tv;
}

/**
 * ns_to_timespec64 - Convert nanoseconds to timespec64
 * @nsec:       the nanoseconds value to be converted
 *
 * Returns the timespec64 representation of the nsec parameter.
 */
struct timespec64 ns_to_timespec64(const s64 nsec)
{
	struct timespec64 ts;
	s32 rem;

	if (!nsec)
		return (struct timespec64) {0, 0};

	ts.tv_sec = div_s64_rem(nsec, NSEC_PER_SEC, &rem);
	if (unlikely(rem < 0)) {
		ts.tv_sec--;
		rem += NSEC_PER_SEC;
	}
	ts.tv_nsec = rem;

	return ts;
}
