#ifndef LIBKLIB_CMPXCHG_H
#define LIBKLIB_CMPXCHG_H

#include <libklib/atomic-gcc.h>

#define xchg(ptr, v)	klib_atomic_exchange_n(ptr, v, __ATOMIC_SEQ_CST)

#define cmpxchg(ptr, old, new)	({					\
	__typeof__ (*(ptr)) __exp = (old);				\
	klib_atomic_compare_exchange_n(ptr, &__exp, new, false,		\
				    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
	__exp;								\
})

#define cmpxchg_local(ptr, old, new)	cmpxchg(ptr, old, new)

#endif /* !LIBKLIB_CMPXCHG_H */
