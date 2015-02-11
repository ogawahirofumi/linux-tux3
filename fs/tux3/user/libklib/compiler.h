#ifndef LIBKLIB_COMPILER_H
#define LIBKLIB_COMPILER_H

#ifdef __CHECKER__
# define __kernel	__attribute__((address_space(0)))
# define __force	__attribute__((force))
# define __rcu		__attribute__((noderef, address_space(4)))
#else
# define __kernel
# define __force
# define __rcu
#endif

/* Indirect macros required for expanded argument pasting, eg. __LINE__. */
#define ___PASTE(a,b) a##b
#define __PASTE(a,b) ___PASTE(a,b)

#ifdef __GNUC__
#include <libklib/compiler-gcc.h>
#endif

#define notrace

/* Intel compiler defines __GNUC__. So we will overwrite implementations
 * coming from above header files here
 */
#ifdef __INTEL_COMPILER
# include <libklib/compiler-intel.h>
#endif

/* Clang compiler defines __GNUC__. So we will overwrite implementations
 * coming from above header files here
 */
#ifdef __clang__
#include <libklib/compiler-clang.h>
#endif

# define likely(x)	__builtin_expect(!!(x), 1)
# define unlikely(x)	__builtin_expect(!!(x), 0)

/* Optimization barrier */
#ifndef barrier
# define barrier() __memory_barrier()
#endif

/* Unreachable code */
#ifndef unreachable
# define unreachable() do { } while (1)
#endif

/* Not-quite-unique ID. */
#ifndef __UNIQUE_ID
# define __UNIQUE_ID(prefix) __PASTE(__PASTE(__UNIQUE_ID_, prefix), __LINE__)
#endif

#include <libklib/types.h>

static __always_inline void data_access_exceeds_word_size(void)
#ifdef __compiletime_warning
__compiletime_warning("data access exceeds word size and won't be atomic")
#endif
;

static __always_inline void data_access_exceeds_word_size(void)
{
}

static __always_inline void __read_once_size(volatile void *p, void *res, int size)
{
	switch (size) {
	case 1: *(__u8 *)res = *(volatile __u8 *)p; break;
	case 2: *(__u16 *)res = *(volatile __u16 *)p; break;
	case 4: *(__u32 *)res = *(volatile __u32 *)p; break;
#ifdef CONFIG_64BIT
	case 8: *(__u64 *)res = *(volatile __u64 *)p; break;
#endif
	default:
		barrier();
		__builtin_memcpy((void *)res, (const void *)p, size);
		data_access_exceeds_word_size();
		barrier();
	}
}

static __always_inline void __write_once_size(volatile void *p, void *res, int size)
{
	switch (size) {
	case 1: *(volatile __u8 *)p = *(__u8 *)res; break;
	case 2: *(volatile __u16 *)p = *(__u16 *)res; break;
	case 4: *(volatile __u32 *)p = *(__u32 *)res; break;
#ifdef CONFIG_64BIT
	case 8: *(volatile __u64 *)p = *(__u64 *)res; break;
#endif
	default:
		barrier();
		__builtin_memcpy((void *)p, (const void *)res, size);
		data_access_exceeds_word_size();
		barrier();
	}
}

/*
 * Prevent the compiler from merging or refetching reads or writes. The
 * compiler is also forbidden from reordering successive instances of
 * READ_ONCE, WRITE_ONCE and ACCESS_ONCE (see below), but only when the
 * compiler is aware of some particular ordering.  One way to make the
 * compiler aware of ordering is to put the two invocations of READ_ONCE,
 * WRITE_ONCE or ACCESS_ONCE() in different C statements.
 *
 * In contrast to ACCESS_ONCE these two macros will also work on aggregate
 * data types like structs or unions. If the size of the accessed data
 * type exceeds the word size of the machine (e.g., 32 bits or 64 bits)
 * READ_ONCE() and WRITE_ONCE()  will fall back to memcpy and print a
 * compile-time warning.
 *
 * Their two major use cases are: (1) Mediating communication between
 * process-level code and irq/NMI handlers, all running on the same CPU,
 * and (2) Ensuring that the compiler does not  fold, spindle, or otherwise
 * mutilate accesses that either do not require ordering or that interact
 * with an explicit memory barrier or atomic instruction that provides the
 * required ordering.
 */

#define READ_ONCE(x) \
	({ typeof(x) __val; __read_once_size(&x, &__val, sizeof(__val)); __val; })

#define WRITE_ONCE(x, val) \
	({ typeof(x) __val; __val = val; __write_once_size(&x, &__val, sizeof(__val)); __val; })

#ifndef __deprecated
# define __deprecated		/* unimplemented */
#endif

#define __deprecated_for_modules

#ifndef __must_check
#define __must_check
#endif

#ifndef __used
# define __used			/* unimplemented */
#endif

#ifndef __maybe_unused
# define __maybe_unused		/* unimplemented */
#endif

#ifndef __always_unused
# define __always_unused	/* unimplemented */
#endif

#ifndef noinline
#define noinline
#endif

#define noinline_for_stack noinline

#ifndef __always_inline
#define __always_inline inline
#endif

#ifndef __attribute_const__
# define __attribute_const__	/* unimplemented */
#endif

#ifndef __cold
#define __cold
#endif

/* Simple shorthand for a section definition */
#ifndef __section
# define __section(S) __attribute__ ((__section__(#S)))
#endif

#ifndef __visible
#define __visible
#endif

/* Are two types/vars the same type (ignoring qualifiers)? */
#ifndef __same_type
# define __same_type(a, b) __builtin_types_compatible_p(typeof(a), typeof(b))
#endif

/* Is this type a native word size -- useful for atomic operations */
#ifndef __native_word
# define __native_word(t) (sizeof(t) == sizeof(int) || sizeof(t) == sizeof(long))
#endif

/* Compile time object size, -1 for unknown */
#ifndef __compiletime_object_size
# define __compiletime_object_size(obj) -1
#endif
#ifndef __compiletime_warning
# define __compiletime_warning(message)
#endif
#ifndef __compiletime_error
# define __compiletime_error(message)
/*
 * Sparse complains of variable sized arrays due to the temporary variable in
 * __compiletime_assert. Unfortunately we can't just expand it out to make
 * sparse see a constant array size without breaking compiletime_assert on old
 * versions of GCC (e.g. 4.2.4), so hide the array from sparse altogether.
 */
# ifndef __CHECKER__
#  define __compiletime_error_fallback(condition) \
	do { ((void)sizeof(char[1 - 2 * condition])); } while (0)
# endif
#endif
#ifndef __compiletime_error_fallback
# define __compiletime_error_fallback(condition) do { } while (0)
#endif

#define __compiletime_assert(condition, msg, prefix, suffix)		\
	do {								\
		bool __cond = !(condition);				\
		extern void prefix ## suffix(void) __compiletime_error(msg); \
		if (__cond)						\
			prefix ## suffix();				\
		__compiletime_error_fallback(__cond);			\
	} while (0)

#define _compiletime_assert(condition, msg, prefix, suffix) \
	__compiletime_assert(condition, msg, prefix, suffix)

/**
 * compiletime_assert - break build and emit msg if condition is false
 * @condition: a compile-time constant condition to check
 * @msg:       a message to emit if condition is false
 *
 * In tradition of POSIX assert, this macro will break the build if the
 * supplied condition is *false*, emitting the supplied error message if the
 * compiler has support to do so.
 */
#define compiletime_assert(condition, msg) \
	_compiletime_assert(condition, msg, __compiletime_assert_, __LINE__)

#define compiletime_assert_atomic_type(t)				\
	compiletime_assert(__native_word(t),				\
		"Need native word sized stores/loads for atomicity.")

/*
 * Prevent the compiler from merging or refetching accesses.  The compiler
 * is also forbidden from reordering successive instances of ACCESS_ONCE(),
 * but only when the compiler is aware of some particular ordering.  One way
 * to make the compiler aware of ordering is to put the two invocations of
 * ACCESS_ONCE() in different C statements.
 *
 * This macro does absolutely -nothing- to prevent the CPU from reordering,
 * merging, or refetching absolutely anything at any time.  Its main intended
 * use is to mediate communication between process-level code and irq/NMI
 * handlers, all running on the same CPU.
 */
#define ACCESS_ONCE(x) (*(volatile typeof(x) *)&(x))

#endif /* !LIBKLIB_COMPILER_H */
