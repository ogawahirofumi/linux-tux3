/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_GENERIC_BITS_PER_LONG
#define __ASM_GENERIC_BITS_PER_LONG

#include <limits.h>

#define BITS_PER_LONG		LONG_BIT	/* SuS define this */

#ifndef BITS_PER_LONG_LONG
#define BITS_PER_LONG_LONG 64
#endif

#endif /* __ASM_GENERIC_BITS_PER_LONG */
