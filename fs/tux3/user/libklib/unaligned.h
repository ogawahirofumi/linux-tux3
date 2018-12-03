#ifndef LIBKLIB_GENERIC_UNALIGNED_H
#define LIBKLIB_GENERIC_UNALIGNED_H

/*
 * This is the most generic implementation of unaligned accesses
 * and should work almost anywhere.
 */

#if defined(__i386__) || defined (__x86_64__)
#ifndef CONFIG_MEMMOVE_UNALIGNED_ACCESS
# define CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS	1
#endif
#endif

/* Set by the arch if it can handle unaligned accesses in hardware. */
#ifdef CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS
# include <libklib/unaligned/access_ok.h>
#endif

#if __BYTE_ORDER == __LITTLE_ENDIAN
# ifndef CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS
#  ifdef CONFIG_MEMMOVE_UNALIGNED_ACCESS
#   include <libklib/unaligned/le_memmove.h>
#   include <libklib/unaligned/be_byteshift.h>
#  else
#   include <libklib/unaligned/le_struct.h>
#   include <libklib/unaligned/be_byteshift.h>
#  endif
# endif
# include <libklib/unaligned/generic.h>
# define get_unaligned	__get_unaligned_le
# define put_unaligned	__put_unaligned_le
#elif __BYTE_ORDER == __BIG_ENDIAN
# ifndef CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS
#  ifdef CONFIG_MEMMOVE_UNALIGNED_ACCESS
#   include <libklib/unaligned/be_memmove.h>
#   include <libklib/unaligned/le_byteshift.h>
#  else
#   include <libklib/unaligned/be_struct.h>
#   include <libklib/unaligned/le_byteshift.h>
#  endif
# endif
# include <libklib/unaligned/generic.h>
# define get_unaligned	__get_unaligned_be
# define put_unaligned	__put_unaligned_be
#else
# error need to define endianess
#endif

#endif /* !LIBKLIB_GENERIC_UNALIGNED_H */
