/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LIBKLIB__ASM_GENERIC_BITS_PER_LONG
#define LIBKLIB__ASM_GENERIC_BITS_PER_LONG

#include <limits.h>
#include <asm/bitsperlong.h>

#if __BITS_PER_LONG != LONG_BIT
#error "__BITS_PER_LONG != LONG_BIT"
#endif

#define BITS_PER_LONG		LONG_BIT	/* SuS define this */

#if BITS_PER_LONG == 64
#define CONFIG_64BIT		1
#endif

#ifndef BITS_PER_LONG_LONG
#define BITS_PER_LONG_LONG 64
#endif

/*
 * small_const_nbits(n) is true precisely when it is known at compile-time
 * that BITMAP_SIZE(n) is 1, i.e. 1 <= n <= BITS_PER_LONG. This allows
 * various bit/bitmap APIs to provide a fast inline implementation. Bitmaps
 * of size 0 are very rare, and a compile-time-known-size 0 is most likely
 * a sign of error. They will be handled correctly by the bit/bitmap APIs,
 * but using the out-of-line functions, so that the inline implementations
 * can unconditionally dereference the pointer(s).
 */
#define small_const_nbits(nbits) \
	(__builtin_constant_p(nbits) && (nbits) <= BITS_PER_LONG && (nbits) > 0)

#endif /* LIBKLIB__ASM_GENERIC_BITS_PER_LONG */
