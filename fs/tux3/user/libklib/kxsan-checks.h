#ifndef LIBKLIB_KXSCAN_CHECKS_H
#define LIBKLIB_KXSCAN_CHECKS_H

#include <libklib/types.h>

/*
 * Based on linux/include/linux/kasan-checks.h
 */

static inline bool __kasan_check_read(const volatile void *p, unsigned int size)
{
	return true;
}
static inline bool __kasan_check_write(const volatile void *p, unsigned int size)
{
	return true;
}

static inline bool kasan_check_read(const volatile void *p, unsigned int size)
{
	return true;
}
static inline bool kasan_check_write(const volatile void *p, unsigned int size)
{
	return true;
}


/*
 * Based on linux/include/linux/kcsan-checks.h
 */

/* Access types -- if KCSAN_ACCESS_WRITE is not set, the access is a read. */
#define KCSAN_ACCESS_WRITE	(1 << 0) /* Access is a write. */
#define KCSAN_ACCESS_COMPOUND	(1 << 1) /* Compounded read-write instrumentation. */
#define KCSAN_ACCESS_ATOMIC	(1 << 2) /* Access is atomic. */
/* The following are special, and never due to compiler instrumentation. */
#define KCSAN_ACCESS_ASSERT	(1 << 3) /* Access is an assertion. */
#define KCSAN_ACCESS_SCOPED	(1 << 4) /* Access is a scoped access. */

static inline void __kcsan_check_access(const volatile void *ptr, size_t size,
					int type) { }

static inline void kcsan_disable_current(void)		{ }
static inline void kcsan_enable_current(void)		{ }
static inline void kcsan_enable_current_nowarn(void)	{ }
static inline void kcsan_nestable_atomic_begin(void)	{ }
static inline void kcsan_nestable_atomic_end(void)	{ }
static inline void kcsan_flat_atomic_begin(void)	{ }
static inline void kcsan_flat_atomic_end(void)		{ }
static inline void kcsan_atomic_next(int n)		{ }
static inline void kcsan_set_access_mask(unsigned long mask) { }

struct kcsan_scoped_access { };
#define __kcsan_cleanup_scoped __maybe_unused
static inline struct kcsan_scoped_access *
kcsan_begin_scoped_access(const volatile void *ptr, size_t size, int type,
			  struct kcsan_scoped_access *sa) { return sa; }
static inline void kcsan_end_scoped_access(struct kcsan_scoped_access *sa) { }

static inline void kcsan_check_access(const volatile void *ptr, size_t size,
				      int type) { }
static inline void __kcsan_enable_current(void)  { }
static inline void __kcsan_disable_current(void) { }

#define __kcsan_check_read(ptr, size) __kcsan_check_access(ptr, size, 0)
#define __kcsan_check_write(ptr, size)                                         \
	__kcsan_check_access(ptr, size, KCSAN_ACCESS_WRITE)
#define __kcsan_check_read_write(ptr, size)                                    \
	__kcsan_check_access(ptr, size, KCSAN_ACCESS_COMPOUND | KCSAN_ACCESS_WRITE)
#define kcsan_check_read(ptr, size) kcsan_check_access(ptr, size, 0)
#define kcsan_check_write(ptr, size)                                           \
	kcsan_check_access(ptr, size, KCSAN_ACCESS_WRITE)
#define kcsan_check_read_write(ptr, size)                                      \
	kcsan_check_access(ptr, size, KCSAN_ACCESS_COMPOUND | KCSAN_ACCESS_WRITE)

#ifdef CONFIG_KCSAN_IGNORE_ATOMICS
#define kcsan_check_atomic_read(...)		do { } while (0)
#define kcsan_check_atomic_write(...)		do { } while (0)
#define kcsan_check_atomic_read_write(...)	do { } while (0)
#else
#define kcsan_check_atomic_read(ptr, size)                                     \
	kcsan_check_access(ptr, size, KCSAN_ACCESS_ATOMIC)
#define kcsan_check_atomic_write(ptr, size)                                    \
	kcsan_check_access(ptr, size, KCSAN_ACCESS_ATOMIC | KCSAN_ACCESS_WRITE)
#define kcsan_check_atomic_read_write(ptr, size)                               \
	kcsan_check_access(ptr, size, KCSAN_ACCESS_ATOMIC | KCSAN_ACCESS_WRITE | KCSAN_ACCESS_COMPOUND)
#endif

#endif /* !LIBKLIB_KXSCAN_CHECKS_H */
