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

#endif /* LIBKLIB__ASM_GENERIC_BITS_PER_LONG */
