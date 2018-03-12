#ifndef LIBKLIB_MATH64_H
#define LIBKLIB_MATH64_H

#include <libklib/types.h>

/**
 * div_s64_rem - signed 64bit divide with 32bit divisor with remainder
 * @dividend: signed 64bit dividend
 * @divisor: signed 32bit divisor
 * @remainder: pointer to signed 32bit remainder
 *
 * Return: sets ``*remainder``, then returns dividend / divisor
 */
static inline s64 div_s64_rem(s64 dividend, s32 divisor, s32 *remainder)
{
	*remainder = dividend % divisor;
	return dividend / divisor;
}

#endif /* !LIBKLIB_MATH64_H */
