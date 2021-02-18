#ifndef LIBKLIB_H
#define LIBKLIB_H

/* Prevent to include linux/types.h */
#define _LINUX_TYPES_H

#include <stddef.h>
#include <stdlib.h>
#include <limits.h>
#include <endian.h>
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>

#include <libklib/typecheck.h>
#include <libklib/export.h>
#include <libklib/math64.h>
#include <libklib/init.h>
#include <libklib/list.h>
#include <libklib/err.h>
#include <libklib/compiler.h>
#include <libklib/types.h>
#include <libklib/limits.h>
#include <libklib/bitops.h>
#include <libklib/bitmap.h>
#include <libklib/byteorder.h>
#include <libklib/unaligned.h>
#include <libklib/build_bug.h>
#include <libklib/hash.h>
#include <libklib/kdev_t.h>
#include <libklib/bug.h>
#include <libklib/list_sort.h>
#include <libklib/barrier.h>
#include <libklib/log2.h>
#include <libklib/math.h>
#include <libklib/minmax.h>
#include <libklib/rcupdate.h>
#include <libklib/wait.h>
#include <libklib/completion.h>
#include <libklib/time.h>
#include <libklib/string.h>
#include <libklib/stringify.h>

#define sizeof_field(TYPE, MEMBER) sizeof((((TYPE *)0)->MEMBER))
#define offsetofend(TYPE, MEMBER) \
	(offsetof(TYPE, MEMBER)	+ sizeof_field(TYPE, MEMBER))


#define __ALIGN_KERNEL(x, a)		__ALIGN_KERNEL_MASK(x, (typeof(x))(a) - 1)
#define __ALIGN_KERNEL_MASK(x, mask)	(((x) + (mask)) & ~(mask))
#define __KERNEL_DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

/**
 * REPEAT_BYTE - repeat the value @x multiple times as an unsigned long value
 * @x: value to repeat
 *
 * NOTE: @x is not checked for > 0xff; larger values produce odd results.
 */
#define REPEAT_BYTE(x)	((~0ul / 0xff) * (x))

/* @a is a power of 2 value */
#define ALIGN(x, a)		__ALIGN_KERNEL((x), (a))
#define ALIGN_DOWN(x, a)	__ALIGN_KERNEL((x) - ((a) - 1), (a))
#define __ALIGN_MASK(x, mask)	__ALIGN_KERNEL_MASK((x), (mask))
#define PTR_ALIGN(p, a)		((typeof(p))ALIGN((unsigned long)(p), (a)))
#define PTR_ALIGN_DOWN(p, a)	((typeof(p))ALIGN_DOWN((unsigned long)(p), (a)))
#define IS_ALIGNED(x, a)		(((x) & ((typeof(x))(a) - 1)) == 0)

/* generic data direction definitions */
#define READ			0
#define WRITE			1

/**
 * ARRAY_SIZE - get the number of elements in array @arr
 * @arr: array to be sized
 */
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]) + __must_be_array(arr))

#define abs64(x)	llabs(x)

#define typeof_member(T, m)	typeof(((T*)0)->m)

/**
 * upper_32_bits - return bits 32-63 of a number
 * @n: the number we're accessing
 *
 * A basic shift-right of a 64- or 32-bit quantity.  Use this to suppress
 * the "right shift count >= width of type" warning when that quantity is
 * 32-bits.
 */
#define upper_32_bits(n) ((u32)(((n) >> 16) >> 16))

/**
 * lower_32_bits - return bits 0-31 of a number
 * @n: the number we're accessing
 */
#define lower_32_bits(n) ((u32)((n) & 0xffffffff))

/* This counts to 12. Any more, it will return 13th argument. */
#define __COUNT_ARGS(_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _n, X...) _n
#define COUNT_ARGS(X...) __COUNT_ARGS(, ##X, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)

#ifndef __CONCAT
#define __CONCAT(a, b) a ## b
#endif
#define CONCATENATE(a, b) __CONCAT(a, b)

/**
 * container_of - cast a member of a structure out to the containing structure
 * @ptr:	the pointer to the member.
 * @type:	the type of the container struct this is embedded in.
 * @member:	the name of the member within the struct.
 *
 */
#define container_of(ptr, type, member) ({				\
	void *__mptr = (void *)(ptr);					\
	BUILD_BUG_ON_MSG(!__same_type(*(ptr), ((type *)0)->member) &&	\
			 !__same_type(*(ptr), void),			\
			 "pointer type mismatch in container_of()");	\
	((type *)(__mptr - offsetof(type, member))); })

/**
 * container_of_safe - cast a member of a structure out to the containing structure
 * @ptr:	the pointer to the member.
 * @type:	the type of the container struct this is embedded in.
 * @member:	the name of the member within the struct.
 *
 * If IS_ERR_OR_NULL(ptr), ptr is returned unchanged.
 */
#define container_of_safe(ptr, type, member) ({				\
	void *__mptr = (void *)(ptr);					\
	BUILD_BUG_ON_MSG(!__same_type(*(ptr), ((type *)0)->member) &&	\
			 !__same_type(*(ptr), void),			\
			 "pointer type mismatch in container_of()");	\
	IS_ERR_OR_NULL(__mptr) ? ERR_CAST(__mptr) :			\
		((type *)(__mptr - offsetof(type, member))); })

#define S_IRWXUGO	(S_IRWXU|S_IRWXG|S_IRWXO)
#define S_IALLUGO	(S_ISUID|S_ISGID|S_ISVTX|S_IRWXUGO)
#define S_IRUGO		(S_IRUSR|S_IRGRP|S_IROTH)
#define S_IWUGO		(S_IWUSR|S_IWGRP|S_IWOTH)
#define S_IXUGO		(S_IXUSR|S_IXGRP|S_IXOTH)

#endif /* !LIBKLIB_H */
