#ifndef LIBKLIB_BARRIER_H
#define LIBKLIB_BARRIER_H

/*
 * Force strict CPU ordering.
 * And yes, this is required on UP too when we're talking
 * to devices.
 *
 * This implementation only contains a compiler barrier.
 */

#define mb()		barrier()
#define rmb()		mb()
#define wmb()		barrier()
#define dma_rmb()	rmb()
#define dma_wmb()	wmb()

#ifdef CONFIG_SMP
#define smp_mb()	mb()
#define smp_rmb()	rmb()
#define smp_wmb()	wmb()
#else
#define smp_mb()	barrier()
#define smp_rmb()	barrier()
#define smp_wmb()	barrier()
#endif

#define set_mb(var, value)  do { var = value;  mb(); } while (0)

#define read_barrier_depends()		do {} while (0)
#define smp_read_barrier_depends()	do {} while (0)

#define smp_mb__before_atomic()	smp_mb()
#define smp_mb__after_atomic()	smp_mb()

#define smp_store_release(p, v)						\
do {									\
	compiletime_assert_atomic_type(*p);				\
	smp_mb();							\
	ACCESS_ONCE(*p) = (v);						\
} while (0)

#define smp_load_acquire(p)						\
({									\
	typeof(*p) ___p1 = ACCESS_ONCE(*p);				\
	compiletime_assert_atomic_type(*p);				\
	smp_mb();							\
	___p1;								\
})

#endif /* !LIBKLIB_BARRIER_H */
