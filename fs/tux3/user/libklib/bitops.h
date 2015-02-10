#ifndef LIBKLIB_BITOPS_H
#define LIBKLIB_BITOPS_H

#include <limits.h>

#define BITS_PER_LONG		LONG_BIT	/* SuS define this */
#define BITS_PER_LONG_LONG	64

#define BIT(nr)			(1UL << (nr))
#define BIT_ULL(nr)		(1ULL << (nr))
#define BIT_MASK(nr)		(1UL << ((nr) % BITS_PER_LONG))
#define BIT_WORD(nr)		((nr) / BITS_PER_LONG)
#define BIT_ULL_MASK(nr)	(1ULL << ((nr) % BITS_PER_LONG_LONG))
#define BIT_ULL_WORD(nr)	((nr) / BITS_PER_LONG_LONG)
#define BITS_PER_BYTE		8
#define BITS_TO_LONGS(nr)	DIV_ROUND_UP(nr, BITS_PER_BYTE * sizeof(long))

/*
 * Create a contiguous bitmask starting at bit position @l and ending at
 * position @h. For example
 * GENMASK_ULL(39, 21) gives us the 64bit vector 0x000000ffffe00000.
 */
#define GENMASK(h, l) \
	(((~0UL) << (l)) & (~0UL >> (BITS_PER_LONG - 1 - (h))))

#define GENMASK_ULL(h, l) \
	(((~0ULL) << (l)) & (~0ULL >> (BITS_PER_LONG_LONG - 1 - (h))))

#include <libklib/bitops/__ffs.h>
#include <libklib/bitops/ffz.h>
#include <libklib/bitops/fls.h>
#include <libklib/bitops/__fls.h>
#include <libklib/bitops/fls64.h>
#include <libklib/bitops/find.h>

#include <libklib/bitops/atomic.h>
#include <libklib/bitops/non-atomic.h>
#include <libklib/bitops/le.h>

static inline unsigned fls_long(unsigned long l)
{
	if (sizeof(l) == 4)
		return fls(l);
	return fls64(l);
}

#endif /* !LIBKLIB_BITOPS_H */
