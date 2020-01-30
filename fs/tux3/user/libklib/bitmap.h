/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LIBKLIB_BITMAP_H
#define LIBKLIB_BITMAP_H

#include <libklib/types.h>
#include <libklib/bitops.h>
#include <libklib/string.h>

/**
 * bitmap_get_value8 - get an 8-bit value within a memory region
 * @map: address to the bitmap memory region
 * @start: bit offset of the 8-bit value; must be a multiple of 8
 *
 * Returns the 8-bit value located at the @start bit offset within the @src
 * memory region.
 */
static inline unsigned long bitmap_get_value8(const unsigned long *map,
					      unsigned long start)
{
	const size_t index = BIT_WORD(start);
	const unsigned long offset = start % BITS_PER_LONG;

	return (map[index] >> offset) & 0xFF;
}

/**
 * bitmap_set_value8 - set an 8-bit value within a memory region
 * @map: address to the bitmap memory region
 * @value: the 8-bit value; values wider than 8 bits may clobber bitmap
 * @start: bit offset of the 8-bit value; must be a multiple of 8
 */
static inline void bitmap_set_value8(unsigned long *map, unsigned long value,
				     unsigned long start)
{
	const size_t index = BIT_WORD(start);
	const unsigned long offset = start % BITS_PER_LONG;

	map[index] &= ~(0xFFUL << offset);
	map[index] |= value << offset;
}

#endif /* !LIBKLIB_BITMAP_H */
