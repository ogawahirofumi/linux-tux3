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

/* Atomic operations are already serializing on x86 */
#define smp_mb__before_atomic()	barrier()
#define smp_mb__after_atomic()	barrier()

#endif /* !LIBKLIB_BARRIER_H */
