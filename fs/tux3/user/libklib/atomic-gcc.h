#ifndef LIBKLIB_ATOMIC_GCC_H
#define LIBKLIB_ATOMIC_GCC_H

#ifdef LIBKLIB_USE_ATOMIC
/* atomic operations */

#ifdef __ATOMIC_SEQ_CST
/* use gcc builtin __atomic_ */

#define klib_atomic_fetch_add(...)	__atomic_fetch_add(__VA_ARGS__)
#define klib_atomic_fetch_sub(...)	__atomic_fetch_sub(__VA_ARGS__)
#define klib_atomic_fetch_and(...)	__atomic_fetch_and(__VA_ARGS__)
#define klib_atomic_fetch_or(...)	__atomic_fetch_or(__VA_ARGS__)
#define klib_atomic_fetch_xor(...)	__atomic_fetch_xor(__VA_ARGS__)
#define klib_atomic_add_fetch(...)	__atomic_add_fetch(__VA_ARGS__)
#define klib_atomic_sub_fetch(...)	__atomic_sub_fetch(__VA_ARGS__)
#define klib_atomic_and_fetch(...)	__atomic_and_fetch(__VA_ARGS__)
#define klib_atomic_or_fetch(...)	__atomic_or_fetch(__VA_ARGS__)
#define klib_atomic_xor_fetch(...)	__atomic_xor_fetch(__VA_ARGS__)

#define klib_atomic_exchange_n(...)		\
	__atomic_exchange_n(__VA_ARGS__)

#define klib_atomic_compare_exchange_n(...)	\
	__atomic_compare_exchange_n(__VA_ARGS__)

#else /* !__ATOMIC_SEQ_CST */
/* use gcc builtin __sync_ */

#define klib_atomic_fetch_add(ptr, arg, order) __sync_fetch_and_add(ptr, arg)
#define klib_atomic_fetch_sub(ptr, arg, order) __sync_fetch_and_sub(ptr, arg)
#define klib_atomic_fetch_and(ptr, arg, order) __sync_fetch_and_and(ptr, arg)
#define klib_atomic_fetch_or(ptr, arg, order)  __sync_fetch_and_or(ptr, arg)
#define klib_atomic_fetch_xor(ptr, arg, order) __sync_fetch_and_xor(ptr, arg)
#define klib_atomic_add_fetch(ptr, arg, order) __sync_add_and_fetch(ptr, arg)
#define klib_atomic_sub_fetch(ptr, arg, order) __sync_sub_and_fetch(ptr, arg)
#define klib_atomic_and_fetch(ptr, arg, order) __sync_and_and_fetch(ptr, arg)
#define klib_atomic_or_fetch(ptr, arg, order)  __sync_or_and_fetch(ptr, arg)
#define klib_atomic_xor_fetch(ptr, arg, order) __sync_xor_and_fetch(ptr, arg)

#define klib_atomic_exchange_n(ptr, arg, order)				\
({									\
	__typeof__ (*(ptr)) *__ptr = (ptr);				\
	__typeof__ (*(ptr)) __arg = (arg);				\
	__typeof__ (*(ptr)) __exp = *__ptr;				\
	__typeof__ (*(ptr)) __old;					\
	while (1) {							\
		__old = __sync_val_compare_and_swap(__ptr, __exp, __arg); \
		if (__exp == __old)					\
			break;						\
		__exp = __old;						\
	}								\
	__old;								\
})

#define klib_atomic_compare_exchange_n(ptr, old, new, weak, order1, order2) \
({									\
	__typeof__ (*(ptr)) *__ptr = (ptr);				\
	__typeof__ (*(ptr)) *__old = (old);				\
	__typeof__ (*(ptr)) __exp = *__old;				\
	*__old = __sync_val_compare_and_swap(__ptr, __exp, new);	\
	__exp == *__old;						\
})

#endif /* !__ATOMIC_SEQ_CST */

#else /* !LIBKLIB_USE_ATOMIC */
/* Not atomic operations actually */

#define __NO_ATOMIC_FETCH_OP(op, ptr, arg, order)		\
({								\
	__typeof__ (*(ptr)) *__ptr = (ptr);			\
	__typeof__ (*(ptr)) __arg = (arg);			\
	__typeof__ (*(ptr)) __old = *__ptr;			\
	*__ptr = *__ptr op __arg;				\
	__old;							\
})

#define __NO_ATOMIC_OP_FETCH(op, ptr, arg, order)		\
({								\
	__typeof__ (*(ptr)) *__ptr = (ptr);			\
	__typeof__ (*(ptr)) __arg = (arg);			\
	*__ptr = *__ptr op __arg;				\
	*__ptr;							\
})

#define klib_atomic_fetch_add(...)	__NO_ATOMIC_FETCH_OP(+, __VA_ARGS__)
#define klib_atomic_fetch_sub(...)	__NO_ATOMIC_FETCH_OP(-, __VA_ARGS__)
#define klib_atomic_fetch_and(...)	__NO_ATOMIC_FETCH_OP(&, __VA_ARGS__)
#define klib_atomic_fetch_or(...)	__NO_ATOMIC_FETCH_OP(|, __VA_ARGS__)
#define klib_atomic_fetch_xor(...)	__NO_ATOMIC_FETCH_OP(^, __VA_ARGS__)
#define klib_atomic_add_fetch(...)	__NO_ATOMIC_OP_FETCH(+, __VA_ARGS__)
#define klib_atomic_sub_fetch(...)	__NO_ATOMIC_OP_FETCH(-, __VA_ARGS__)
#define klib_atomic_and_fetch(...)	__NO_ATOMIC_OP_FETCH(&, __VA_ARGS__)
#define klib_atomic_or_fetch(...)	__NO_ATOMIC_OP_FETCH(|, __VA_ARGS__)
#define klib_atomic_xor_fetch(...)	__NO_ATOMIC_OP_FETCH(^, __VA_ARGS__)

#define klib_atomic_exchange_n(ptr, arg, order)			\
({								\
	__typeof__ (*(ptr)) *__ptr = (ptr);			\
	__typeof__ (*(ptr)) __arg = (arg);			\
	__typeof__ (*(ptr)) __old = *__ptr;				\
	*__ptr = __arg;						\
	__old;							\
})

#define klib_atomic_compare_exchange_n(ptr, old, new, weak, order1, order2) \
({									\
	__typeof__ (*(ptr)) *__ptr = (ptr);				\
	__typeof__ (*(ptr)) *__old = (old);				\
	__typeof__ (*(ptr)) __new = (new);				\
	__typeof__ (*(ptr)) __prev = *__ptr;				\
	bool __ret;							\
	if (__prev == *__old) {						\
		*__ptr = __new;						\
		__ret = true;						\
	} else {							\
		*__old = __prev;					\
		__ret = false;						\
	}								\
	__ret;								\
})

#endif /* !LIBKLIB_USE_ATOMIC */

#ifndef __ATOMIC_SEQ_CST
#define __ATOMIC_RELAXED	1
#define __ATOMIC_CONSUME	2
#define __ATOMIC_ACQUIRE	3
#define __ATOMIC_RELEASE	4
#define __ATOMIC_ACQ_REL	5
#define __ATOMIC_SEQ_CST	6
#endif

#endif /* !LIBKLIB_ATOMIC_GCC_H */
