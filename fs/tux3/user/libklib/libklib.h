#ifndef LIBKLIB_H
#define LIBKLIB_H

/* Prevent to include linux/types.h */
#define _LINUX_TYPES_H

#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <limits.h>
#include <endian.h>
#include <errno.h>
#include <sys/types.h>

#include <libklib/typecheck.h>
#include <libklib/math64.h>
#include <libklib/init.h>
#include <libklib/list.h>
#include <libklib/err.h>
#include <libklib/compiler.h>
#include <libklib/types.h>
#include <libklib/bitops.h>
#include <libklib/byteorder.h>
#include <libklib/unaligned.h>
#include <libklib/build_bug.h>
#include <libklib/hash.h>
#include <libklib/kdev_t.h>
#include <libklib/bug.h>
#include <libklib/list_sort.h>
#include <libklib/barrier.h>
#include <libklib/log2.h>
#include <libklib/rcupdate.h>
#include <libklib/wait.h>
#include <libklib/completion.h>
#include <libklib/time.h>
#include <libklib/stringify.h>

#define __ALIGN_KERNEL(x, a)		__ALIGN_KERNEL_MASK(x, (typeof(x))(a) - 1)
#define __ALIGN_KERNEL_MASK(x, mask)	(((x) + (mask)) & ~(mask))
#define __KERNEL_DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

/* @a is a power of 2 value */
#define ALIGN(x, a)		__ALIGN_KERNEL((x), (a))
#define ALIGN_DOWN(x, a)	__ALIGN_KERNEL((x) - ((a) - 1), (a))
#define __ALIGN_MASK(x, mask)	__ALIGN_KERNEL_MASK((x), (mask))
#define PTR_ALIGN(p, a)		((typeof(p))ALIGN((unsigned long)(p), (a)))
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

/*
 * This looks more complex than it should be. But we need to
 * get the type for the ~ right in round_down (it needs to be
 * as wide as the result!), and we want to evaluate the macro
 * arguments just once each.
 */
#define __round_mask(x, y) ((__typeof__(x))((y)-1))
#define round_up(x, y) ((((x)-1) | __round_mask(x, y))+1)
#define round_down(x, y) ((x) & ~__round_mask(x, y))

/**
 * FIELD_SIZEOF - get the size of a struct's field
 * @t: the target struct
 * @f: the target struct's field
 * Return: the size of @f in the struct definition without having a
 * declared instance of @t.
 */
#define FIELD_SIZEOF(t, f) (sizeof(((t*)0)->f))

#define DIV_ROUND_UP __KERNEL_DIV_ROUND_UP

/*
 * min()/max()/clamp() macros that also do
 * strict type-checking.. See the
 * "unnecessary" pointer comparison.
 */
#define __min(t1, t2, min1, min2, x, y) ({		\
	t1 min1 = (x);					\
	t2 min2 = (y);					\
	(void) (&min1 == &min2);			\
	min1 < min2 ? min1 : min2; })

/**
 * min - return minimum of two values of the same or compatible types
 * @x: first value
 * @y: second value
 */
#define min(x, y)					\
	__min(typeof(x), typeof(y),			\
	      __UNIQUE_ID(min1_), __UNIQUE_ID(min2_),	\
	      x, y)

#define __max(t1, t2, max1, max2, x, y) ({		\
	t1 max1 = (x);					\
	t2 max2 = (y);					\
	(void) (&max1 == &max2);			\
	max1 > max2 ? max1 : max2; })

/**
 * max - return maximum of two values of the same or compatible types
 * @x: first value
 * @y: second value
 */
#define max(x, y)					\
	__max(typeof(x), typeof(y),			\
	      __UNIQUE_ID(max1_), __UNIQUE_ID(max2_),	\
	      x, y)

/**
 * min3 - return minimum of three values
 * @x: first value
 * @y: second value
 * @z: third value
 */
#define min3(x, y, z) min((typeof(x))min(x, y), z)

/**
 * max3 - return maximum of three values
 * @x: first value
 * @y: second value
 * @z: third value
 */
#define max3(x, y, z) max((typeof(x))max(x, y), z)

/**
 * min_not_zero - return the minimum that is _not_ zero, unless both are zero
 * @x: value1
 * @y: value2
 */
#define min_not_zero(x, y) ({			\
	typeof(x) __x = (x);			\
	typeof(y) __y = (y);			\
	__x == 0 ? __y : ((__y == 0) ? __x : min(__x, __y)); })

/**
 * clamp - return a value clamped to a given range with strict typechecking
 * @val: current value
 * @lo: lowest allowable value
 * @hi: highest allowable value
 *
 * This macro does strict typechecking of @lo/@hi to make sure they are of the
 * same type as @val.  See the unnecessary pointer comparisons.
 */
#define clamp(val, lo, hi) min((typeof(val))max(val, lo), hi)

/*
 * ..and if you can't take the strict
 * types, you can specify one yourself.
 *
 * Or not use min/max/clamp at all, of course.
 */

/**
 * min_t - return minimum of two values, using the specified type
 * @type: data type to use
 * @x: first value
 * @y: second value
 */
#define min_t(type, x, y)				\
	__min(type, type,				\
	      __UNIQUE_ID(min1_), __UNIQUE_ID(min2_),	\
	      x, y)

/**
 * max_t - return maximum of two values, using the specified type
 * @type: data type to use
 * @x: first value
 * @y: second value
 */
#define max_t(type, x, y)				\
	__max(type, type,				\
	      __UNIQUE_ID(min1_), __UNIQUE_ID(min2_),	\
	      x, y)

/**
 * clamp_t - return a value clamped to a given range using a given type
 * @type: the type of variable to use
 * @val: current value
 * @lo: minimum allowable value
 * @hi: maximum allowable value
 *
 * This macro does no typechecking and uses temporary variables of type
 * @type to make all the comparisons.
 */
#define clamp_t(type, val, lo, hi) min_t(type, max_t(type, val, lo), hi)

/**
 * clamp_val - return a value clamped to a given range using val's type
 * @val: current value
 * @lo: minimum allowable value
 * @hi: maximum allowable value
 *
 * This macro does no typechecking and uses temporary variables of whatever
 * type the input argument @val is.  This is useful when @val is an unsigned
 * type and @lo and @hi are literals that will otherwise be assigned a signed
 * integer type.
 */
#define clamp_val(val, lo, hi) clamp_t(typeof(val), val, lo, hi)

#define S_IRWXUGO	(S_IRWXU|S_IRWXG|S_IRWXO)
#define S_IALLUGO	(S_ISUID|S_ISGID|S_ISVTX|S_IRWXUGO)
#define S_IRUGO		(S_IRUSR|S_IRGRP|S_IROTH)
#define S_IWUGO		(S_IWUSR|S_IWGRP|S_IWOTH)
#define S_IXUGO		(S_IXUSR|S_IXGRP|S_IXOTH)

#endif /* !LIBKLIB_H */
