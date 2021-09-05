/*
 * Alignment checker on x86.
 */

#include "tux3user.h"
#include "aligncheck.h"

#if defined(ALIGN_CHECK) && (defined(__x86_64__) || defined(__i386__))
static inline unsigned long read_eflags(void)
{
	unsigned long flags;

	/*
	 * "=rm" is safe here, because "pop" adjusts the stack before
	 * it evaluates its effective address -- this is part of the
	 * documented behavior of the "pop" instruction.
	 */
	asm volatile("pushf ; pop %0"
		     : "=rm" (flags)
		     : /* no input */
		     : "memory");

	return flags;
}

static inline void write_eflags(unsigned long flags)
{
	asm volatile("push %0 ; popf"
		     : /* no output */
		     :"g" (flags)
		     :"memory", "cc");
}

void enable_alignment_check(void)
{
#define EFLAGS_AC	(1UL << 18)
	/* Linux kernel sets CR0.AM=1, so changing AC is enough */
	write_eflags(read_eflags() | EFLAGS_AC);
}

void disable_alignment_check(void)
{
	write_eflags(read_eflags() & ~EFLAGS_AC);
}

/*
 * glibc hooks to avoid unaligned access in glibc
 */

#include <dlfcn.h>

#define ALIGN_HOOK(rettype, name, proto, args...)	\
static rettype (*libc_##name) proto;			\
rettype name proto					\
{							\
	static int recursive_##name;			\
	rettype ret;					\
							\
	if (recursive_##name == 0)			\
		disable_alignment_check();		\
	recursive_##name++;				\
							\
	ret = libc_##name(args);			\
							\
	recursive_##name--;				\
	if (recursive_##name == 0)			\
		enable_alignment_check();		\
							\
	return ret;					\
}

ALIGN_HOOK(int, vprintf, (const char *format, va_list ap), format, ap);
ALIGN_HOOK(int, vfprintf, (FILE *stream, const char *format, va_list ap),
	   stream, format, ap);
ALIGN_HOOK(int, puts, (const char *s), s);
ALIGN_HOOK(size_t, fwrite,
	   (const void *ptr, size_t size, size_t nmemb, FILE *stream),
	   ptr, size, nmemb, stream);
ALIGN_HOOK(char *, strerror, (int errnum), errnum);
ALIGN_HOOK(void *, memset, (void *s, int c, size_t n), s, c, n);
ALIGN_HOOK(void *, memmove, (void *dest, const void *src, size_t n),
		   dest, src, n);
ALIGN_HOOK(void *, memcpy, (void *dest, const void *src, size_t n),
		   dest, src, n);
ALIGN_HOOK(int, memcmp, (const void *s1, const void *s2, size_t n), s1, s2, n);
ALIGN_HOOK(int, mkstemp, (char *template), template);

#define ALIGN_HOOK_VAARG(rettype, name, vname, proto, last, args...)	\
rettype name proto							\
{									\
	static int recursive_##name;					\
	va_list ap;							\
	rettype ret;							\
									\
	if (recursive_##name == 0)					\
		disable_alignment_check();				\
	recursive_##name++;						\
									\
	va_start(ap, last);						\
	ret = libc_##vname(args, ap);					\
	va_end(ap);							\
									\
	recursive_##name--;						\
	if (recursive_##name == 0)					\
		enable_alignment_check();				\
									\
	return ret;							\
}

ALIGN_HOOK_VAARG(int, printf, vprintf, (const char *__restrict format, ...),
		 format, format);
ALIGN_HOOK_VAARG(int, fprintf, vfprintf,
		 (FILE *stream, const char *__restrict format, ...),
		 format, stream, format);

void init_alignment_check(void)
{
	static bool initialized;

	if (initialized)
		return;
	initialized = true;

	libc_vprintf = dlsym(RTLD_NEXT, "vprintf");
	libc_vfprintf = dlsym(RTLD_NEXT, "vfprintf");
	libc_puts = dlsym(RTLD_NEXT, "puts");
	libc_fwrite = dlsym(RTLD_NEXT, "fwrite");
	libc_strerror = dlsym(RTLD_NEXT, "strerror");
	libc_memset = dlsym(RTLD_NEXT, "memset");
	libc_memmove = dlsym(RTLD_NEXT, "memmove");
	libc_memcpy = dlsym(RTLD_NEXT, "memcpy");
	libc_memcmp = dlsym(RTLD_NEXT, "memcmp");
	libc_mkstemp = dlsym(RTLD_NEXT, "mkstemp");
	enable_alignment_check();
}
#endif /* ALIGN_CHECK */
