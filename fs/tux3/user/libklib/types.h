#ifndef LIBKLIB_TYPES_H
#define LIBKLIB_TYPES_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef unsigned short		umode_t;

typedef signed char		__s8;
typedef unsigned char		__u8;
typedef signed short		__s16;
typedef unsigned short		__u16;
typedef signed int		__s32;
typedef unsigned int		__u32;
typedef signed long long	__s64;
typedef unsigned long long	__u64;

typedef signed char		s8;
typedef unsigned char		u8;
typedef signed short		s16;
typedef unsigned short		u16;
typedef signed int		s32;
typedef unsigned int		u32;
typedef signed long long	s64;
typedef unsigned long long	u64;

#ifdef __CHECKER__
#define __bitwise__ __attribute__((bitwise))
#else
#define __bitwise__
#endif
#undef __bitwise
#define __bitwise __bitwise__

typedef __u16 __bitwise __le16;
typedef __u16 __bitwise __be16;
typedef __u32 __bitwise __le32;
typedef __u32 __bitwise __be32;
typedef __u64 __bitwise __le64;
typedef __u64 __bitwise __be64;

typedef unsigned __bitwise gfp_t;
typedef unsigned __bitwise slab_flags_t;

#endif /* !LIBKLIB_TYPES_H */
