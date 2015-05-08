#include <stdio.h>
#include <stdlib.h>

#include <tux3user.h>
#include <libklib/libklib.h>
#include <libklib/slab.h>

void *__kmalloc(size_t size, gfp_t flags)
{
	void *p = malloc(size);
	if (p) {
		if (flags & __GFP_ZERO)
			memset(p, 0, size);
	}
	return p;
}

void kfree(const void *p)
{
	if (p)
		free((void *)p);
}

struct kmem_cache *kmem_cache_create(const char *name, size_t size,
				     size_t align, unsigned long flags,
				     void (*ctor)(void *))
{
	struct kmem_cache *cachep;

	cachep = kmalloc(sizeof(*cachep), GFP_KERNEL);
	if (cachep) {
		cachep->name		= name;
		cachep->object_size	= size;
		cachep->align		= align;
		cachep->flags		= flags;
		cachep->ctor		= ctor;
	}
	return cachep;
}

void kmem_cache_destroy(struct kmem_cache *cachep)
{
	kfree(cachep);
}

void kmem_cache_free(struct kmem_cache *cachep, void *objp)
{
	kfree(objp);
}

static void *kmemalign(size_t alignment, size_t size, gfp_t flags)
{
	void *ptr;
	int err;

	err = posix_memalign(&ptr, alignment, size);
	if (err)
		return NULL;

	if (flags & __GFP_ZERO)
		memset(ptr, 0, size);
	return ptr;
}

void *kmem_cache_alloc(struct kmem_cache *cachep, gfp_t flags)
{
	void *objp;

	if (cachep->align)
		objp = kmemalign(cachep->align, cachep->object_size, flags);
	else
		objp = kmalloc(cachep->object_size, flags);

	if (objp) {
		if (cachep->ctor)
			cachep->ctor(objp);
	}

	return objp;
}
