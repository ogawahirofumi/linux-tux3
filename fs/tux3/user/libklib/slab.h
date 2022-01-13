#ifndef LIBKLIB_SLAB_H
#define LIBKLIB_SLAB_H

#include <libklib/mm.h>
#include <libklib/overflow.h>

/*
 * Flags to pass to kmem_cache_create().
 * The ones marked DEBUG are only valid if CONFIG_SLAB_DEBUG is set.
 */
/* DEBUG: Perform (expensive) checks on alloc/free */
#define SLAB_CONSISTENCY_CHECKS	((slab_flags_t __force)0x00000100U)
/* DEBUG: Red zone objs in a cache */
#define SLAB_RED_ZONE		((slab_flags_t __force)0x00000400U)
/* DEBUG: Poison objects */
#define SLAB_POISON		((slab_flags_t __force)0x00000800U)
/* Align objs on cache lines */
#define SLAB_HWCACHE_ALIGN	((slab_flags_t __force)0x00002000U)
/* Use GFP_DMA memory */
#define SLAB_CACHE_DMA		((slab_flags_t __force)0x00004000U)
/* DEBUG: Store the last owner for bug hunting */
#define SLAB_STORE_USER		((slab_flags_t __force)0x00010000U)
/* Panic if kmem_cache_create() fails */
#define SLAB_PANIC		((slab_flags_t __force)0x00040000U)
/*
 * SLAB_TYPESAFE_BY_RCU - **WARNING** READ THIS!
 *
 * This delays freeing the SLAB page by a grace period, it does _NOT_
 * delay object freeing. This means that if you do kmem_cache_free()
 * that memory location is free to be reused at any time. Thus it may
 * be possible to see another object there in the same RCU grace period.
 *
 * This feature only ensures the memory location backing the object
 * stays valid, the trick to using this is relying on an independent
 * object validation pass. Something like:
 *
 *  rcu_read_lock()
 * again:
 *  obj = lockless_lookup(key);
 *  if (obj) {
 *    if (!try_get_ref(obj)) // might fail for free objects
 *      goto again;
 *
 *    if (obj->key != key) { // not the object we expected
 *      put_ref(obj);
 *      goto again;
 *    }
 *  }
 *  rcu_read_unlock();
 *
 * See also the comment on struct slab_rcu in mm/slab.c.
 *
 * Note that SLAB_TYPESAFE_BY_RCU was originally named SLAB_DESTROY_BY_RCU.
 */
/* Defer freeing slabs to RCU */
#define SLAB_TYPESAFE_BY_RCU	((slab_flags_t __force)0x00080000U)
/* Spread some memory over cpuset */
#define SLAB_MEM_SPREAD		((slab_flags_t __force)0x00100000U)
/* Trace allocations and frees */
#define SLAB_TRACE		((slab_flags_t __force)0x00200000U)

/* Flag to prevent checks on free */
#ifdef CONFIG_DEBUG_OBJECTS
# define SLAB_DEBUG_OBJECTS	((slab_flags_t __force)0x00400000U)
#else
# define SLAB_DEBUG_OBJECTS	0
#endif

/* Avoid kmemleak tracing */
#define SLAB_NOLEAKTRACE	((slab_flags_t __force)0x00800000U)

/* Fault injection mark */
#ifdef CONFIG_FAILSLAB
# define SLAB_FAILSLAB		((slab_flags_t __force)0x02000000U)
#else
# define SLAB_FAILSLAB		0
/* Account to memcg */
#endif
#if defined(CONFIG_MEMCG) && !defined(CONFIG_SLOB)
# define SLAB_ACCOUNT		((slab_flags_t __force)0x04000000U)
#else
# define SLAB_ACCOUNT		0
#endif

#ifdef CONFIG_KASAN
#define SLAB_KASAN		((slab_flags_t __force)0x08000000U)
#else
#define SLAB_KASAN		0
#endif

/* The following flags affect the page allocator grouping pages by mobility */
/* Objects are reclaimable */
#define SLAB_RECLAIM_ACCOUNT	((slab_flags_t __force)0x00020000U)
#define SLAB_TEMPORARY		SLAB_RECLAIM_ACCOUNT	/* Objects are short-lived */

struct kmem_cache {
	unsigned int object_size;	/* The original size of the object */
	unsigned int align;		/* Alignment as calculated */
	slab_flags_t flags;		/* Active flags on the slab */
	const char *name;		/* Slab name for sysfs */
	void (*ctor)(void *);		/* Called on object slot creation */
};

void kfree(const void *objp);

struct kmem_cache *kmem_cache_create(const char *name, unsigned int size,
			unsigned int align, slab_flags_t flags,
 			void (*ctor)(void *));
struct kmem_cache *kmem_cache_create_usercopy(const char *name,
			unsigned int size, unsigned int align,
			slab_flags_t flags,
			unsigned int useroffset, unsigned int usersize,
			void (*ctor)(void *));
void kmem_cache_destroy(struct kmem_cache *s);
void kmem_cache_free(struct kmem_cache *, void *s);

#define KMEM_CACHE(__struct, __flags)					\
		kmem_cache_create(#__struct, sizeof(struct __struct),	\
			__alignof__(struct __struct), (__flags), NULL)

/*
 * To whitelist a single field for copying to/from usercopy, use this
 * macro instead for KMEM_CACHE() above.
 */
#define KMEM_CACHE_USERCOPY(__struct, __flags, __field)			\
		kmem_cache_create_usercopy(#__struct,			\
			sizeof(struct __struct),			\
			__alignof__(struct __struct), (__flags),	\
			offsetof(struct __struct, __field),		\
			sizeof_field(struct __struct, __field), NULL)

void *__kmalloc(size_t size, gfp_t flags) __alloc_size(1);
void *kmem_cache_alloc(struct kmem_cache *s, gfp_t flags) __malloc;

static inline __alloc_size(1) void *kmalloc(size_t size, gfp_t flags)
{
	return __kmalloc(size, flags);
}

static inline __alloc_size(1, 2) void *kmalloc_array(size_t n, size_t size, gfp_t flags)
{
	size_t bytes;

	if (unlikely(check_mul_overflow(n, size, &bytes)))
		return NULL;
	return __kmalloc(bytes, flags);
}

static inline __alloc_size(1, 2) void *kcalloc(size_t n, size_t size, gfp_t flags)
{
	return kmalloc_array(n, size, flags | __GFP_ZERO);
}

/*
 * Shortcuts
 */
static inline void *kmem_cache_zalloc(struct kmem_cache *k, gfp_t flags)
{
	return kmem_cache_alloc(k, flags | __GFP_ZERO);
}

static inline __alloc_size(1) void *kzalloc(size_t size, gfp_t flags)
{
	return kmalloc(size, flags | __GFP_ZERO);
}

#endif /* !LIBKLIB_SLAB_H */
