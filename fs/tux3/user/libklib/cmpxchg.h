#ifndef LIBKLIB_CMPXCHG_H
#define LIBKLIB_CMPXCHG_H

#include <libklib/atomic-gcc.h>

#define arch_xchg(ptr, v)	klib_atomic_exchange_n(ptr, v, __ATOMIC_SEQ_CST)

#define arch_cmpxchg(ptr, old, new)	({				\
	__typeof__ (*(ptr)) __exp = (old);				\
	klib_atomic_compare_exchange_n(ptr, &__exp, new, false,		\
				    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
	__exp;								\
})

#define arch_sync_cmpxchg(ptr, old, new)	arch_cmpxchg(ptr, old, new)
#define arch_cmpxchg_local(ptr, old, new)	arch_cmpxchg(ptr, old, new)

#define arch_try_cmpxchg(ptr, pold, new)	({			\
	klib_atomic_compare_exchange_n(ptr, pold, new, false,		\
				    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
})

#define arch_cmpxchg64(ptr, o, n)					\
({									\
	BUILD_BUG_ON(sizeof(*(ptr)) != 8);				\
	cmpxchg((ptr), (o), (n));					\
})

#define arch_cmpxchg64_local(ptr, o, n)					\
({									\
	BUILD_BUG_ON(sizeof(*(ptr)) != 8);				\
	cmpxchg_local((ptr), (o), (n));					\
})

#endif /* !LIBKLIB_CMPXCHG_H */
