#ifndef LIBKLIB_LOCKDEBUG_H
#define LIBKLIB_LOCKDEBUG_H

#include <libklib/atomic.h>
#include <libklib/bug.h>

#define SPINLOCK_MAGIC		0xdead4ead
typedef struct {
#ifdef LOCK_DEBUG
	unsigned int magic;
	int lock;
#endif
} spinlock_t;

#ifdef LOCK_DEBUG
#define __SPIN_LOCK_UNLOCKED(name)		\
	(spinlock_t){ .magic = SPINLOCK_MAGIC, .lock = 0, }
#else
#define __SPIN_LOCK_UNLOCKED(name)		\
	(spinlock_t){ }
#endif
#define DEFINE_SPINLOCK(x)	spinlock_t x = __SPIN_LOCK_UNLOCKED(x)
#define spin_lock_init(lock)	do {		\
	*(lock) = __SPIN_LOCK_UNLOCKED(lock);	\
} while (0)

static inline void _spin_lock(spinlock_t *lock)
{
#ifdef LOCK_DEBUG
	BUG_ON(lock->magic != SPINLOCK_MAGIC);
	BUG_ON(lock->lock != 0);
	lock->lock++;
#endif
}
static inline void spin_lock(spinlock_t *lock) __acquires(lock)
{
	__acquire(lock);
	_spin_lock(lock);
}

static inline void _spin_unlock(spinlock_t *lock)
{
#ifdef LOCK_DEBUG
	BUG_ON(lock->magic != SPINLOCK_MAGIC);
	BUG_ON(lock->lock != 1);
	lock->lock--;
#endif
}
static inline void spin_unlock(spinlock_t *lock) __releases(lock)
{
	_spin_unlock(lock);
	__release(lock);
}

static inline int _spin_trylock(spinlock_t *lock)
{
	_spin_lock(lock);
	return 1;
}
static inline int spin_trylock(spinlock_t *lock)
{
	return __cond_lock(lock, _spin_trylock(lock));
}

static inline int spin_is_locked(spinlock_t *lock)
{
#ifdef LOCK_DEBUG
	return lock->lock != 0;
#else
	return 0;
#endif
}
#ifdef LOCK_DEBUG
#define assert_spin_locked(lock)	BUG_ON(!spin_is_locked(lock))
#else
#define assert_spin_locked(lock)	do {} while (0)
#endif

#define spin_lock_irq(lock)		spin_lock(lock)
#define spin_lock_irqsave(lock, flags)		\
do {						\
	flags = 0;				\
	spin_lock(lock);			\
} while (0)
#define spin_unlock_irq(lock)		spin_unlock(lock)
#define spin_unlock_irqrestore(lock, flags)	\
do {						\
	spin_unlock(lock);			\
	(void)flags;				\
} while (0)

/**
 * atomic_dec_and_lock - lock on reaching reference count zero
 * @atomic: the atomic counter
 * @lock: the spinlock in question
 *
 * Decrements @atomic by 1.  If the result is 0, returns true and locks
 * @lock.  Returns false for all other cases.
 */
static inline int _atomic_dec_and_lock(atomic_t *v, spinlock_t *lock)
{
	_spin_lock(lock);
	if (atomic_dec_and_test(v))
		return 1;
	_spin_unlock(lock);
	return 0;
}
#define atomic_dec_and_lock(atomic, lock) \
		__cond_lock(lock, _atomic_dec_and_lock(atomic, lock))

struct rw_semaphore {
#ifdef LOCK_DEBUG
	unsigned int magic;
	int count;
#endif
};

#ifdef LOCK_DEBUG
#define __RWSEM_INITIALIZER(name)				\
	{ .magic = SPINLOCK_MAGIC, .count = 0, }
#else
#define __RWSEM_INITIALIZER(name)		\
	{ }
#endif
#define DECLARE_RWSEM(name) struct rw_semaphore name = __RWSEM_INITIALIZER(name)
#define init_rwsem(sem) do {					\
	*(sem) = (struct rw_semaphore)__RWSEM_INITIALIZER("");	\
} while (0)

static inline void down_read(struct rw_semaphore *lock)
{
#ifdef LOCK_DEBUG
	BUG_ON(lock->magic != SPINLOCK_MAGIC);
	BUG_ON(lock->count < 0);
	lock->count++;
#endif
}
#define down_read_nested(lock, sub) down_read(lock)
static inline int down_read_trylock(struct rw_semaphore *lock)
{
	down_read(lock);
	return 1;
}
static inline void down_write(struct rw_semaphore *lock)
{
#ifdef LOCK_DEBUG
	BUG_ON(lock->magic != SPINLOCK_MAGIC);
	BUG_ON(lock->count != 0);
	lock->count--;
#endif
}
#define down_write_nested(lock, sub) down_write(lock)
#define down_write_killable(lock, sub) down_write(lock)
static inline int down_write_trylock(struct rw_semaphore *lock)
{
	down_write(lock);
	return 1;
}
static inline void up_read(struct rw_semaphore *lock)
{
#ifdef LOCK_DEBUG
	BUG_ON(lock->magic != SPINLOCK_MAGIC);
	BUG_ON(lock->count < 1);
	lock->count--;
#endif
}
static inline void up_write(struct rw_semaphore *lock)
{
#ifdef LOCK_DEBUG
	BUG_ON(lock->magic != SPINLOCK_MAGIC);
	BUG_ON(lock->count != -1);
	lock->count++;
#endif
}
static inline int rwsem_is_locked(struct rw_semaphore *lock)
{
#ifdef LOCK_DEBUG
	return lock->count != 0;
#else
	return 0;
#endif
}

struct mutex {
#ifdef LOCK_DEBUG
	struct rw_semaphore sem;
#endif
};

#ifdef LOCK_DEBUG
#define __MUTEX_INITIALIZER(lockname) \
	{ .sem = (struct rw_semaphore)__RWSEM_INITIALIZER(lockname), }
#else
#define __MUTEX_INITIALIZER \
	{ }
#endif
#define DEFINE_MUTEX(mutexname)			\
	struct mutex mutexname = __MUTEX_INITIALIZER(mutexname)
#define mutex_init(m) do {				\
	*(m) = (struct mutex)__MUTEX_INITIALIZER("");	\
} while (0)

static inline void mutex_lock(struct mutex *lock)
{
	down_write(&lock->sem);
}
#define mutex_lock_nested(lock, sub) mutex_lock(lock)
#define mutex_lock_interruptible(lock) mutex_lock(lock)
#define mutex_lock_killable(lock) mutex_lock(lock)
static inline int mutex_trylock(struct mutex *lock)
{
	mutex_lock(lock);
	return 1;
}
static inline void mutex_unlock(struct mutex *lock)
{
	up_write(&lock->sem);
}
static inline bool mutex_is_locked(struct mutex *lock)
{
	return rwsem_is_locked(&lock->sem);
}

#endif /* !LIBKLIB_LOCKDEBUG_H */
