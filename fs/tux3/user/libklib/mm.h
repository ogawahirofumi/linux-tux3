#ifndef LIBKLIB_MM_H
#define LIBKLIB_MM_H

/*
 * gfp stuff
 */

/* Plain integer GFP bitmasks. Do not use this directly. */
#define ___GFP_DMA		0x01u
#define ___GFP_HIGHMEM		0x02u
#define ___GFP_DMA32		0x04u
#define ___GFP_MOVABLE		0x08u
#define ___GFP_RECLAIMABLE	0x10u
#define ___GFP_HIGH		0x20u
#define ___GFP_IO		0x40u
#define ___GFP_FS		0x80u
#define ___GFP_NOWARN		0x200u
#define ___GFP_RETRY_MAYFAIL	0x400u
#define ___GFP_NOFAIL		0x800u
#define ___GFP_NORETRY		0x1000u
#define ___GFP_MEMALLOC		0x2000u
#define ___GFP_ZERO		0x8000u
#define ___GFP_NOMEMALLOC	0x10000u
#define ___GFP_HARDWALL		0x20000u
#define ___GFP_ATOMIC		0x80000u
#define ___GFP_ACCOUNT		0x100000u
#define ___GFP_DIRECT_RECLAIM	0x400000u
#define ___GFP_KSWAPD_RECLAIM	0x1000000u

#define __GFP_DMA	((__force gfp_t)___GFP_DMA)
#define __GFP_HIGHMEM	((__force gfp_t)___GFP_HIGHMEM)
#define __GFP_DMA32	((__force gfp_t)___GFP_DMA32)
#define __GFP_MOVABLE	((__force gfp_t)___GFP_MOVABLE)  /* ZONE_MOVABLE allowed */
#define GFP_ZONEMASK	(__GFP_DMA|__GFP_HIGHMEM|__GFP_DMA32|__GFP_MOVABLE)

#define __GFP_RECLAIMABLE ((__force gfp_t)___GFP_RECLAIMABLE)
#define __GFP_HARDWALL   ((__force gfp_t)___GFP_HARDWALL)
#define __GFP_ACCOUNT	((__force gfp_t)___GFP_ACCOUNT)

#define __GFP_ATOMIC	((__force gfp_t)___GFP_ATOMIC)
#define __GFP_HIGH	((__force gfp_t)___GFP_HIGH)
#define __GFP_MEMALLOC	((__force gfp_t)___GFP_MEMALLOC)
#define __GFP_NOMEMALLOC ((__force gfp_t)___GFP_NOMEMALLOC)

#define __GFP_IO	((__force gfp_t)___GFP_IO)
#define __GFP_FS	((__force gfp_t)___GFP_FS)
#define __GFP_DIRECT_RECLAIM	((__force gfp_t)___GFP_DIRECT_RECLAIM) /* Caller can reclaim */
#define __GFP_KSWAPD_RECLAIM	((__force gfp_t)___GFP_KSWAPD_RECLAIM) /* kswapd can wake */
#define __GFP_RECLAIM ((__force gfp_t)(___GFP_DIRECT_RECLAIM|___GFP_KSWAPD_RECLAIM))
#define __GFP_RETRY_MAYFAIL	((__force gfp_t)___GFP_RETRY_MAYFAIL)
#define __GFP_NOFAIL	((__force gfp_t)___GFP_NOFAIL)
#define __GFP_NORETRY	((__force gfp_t)___GFP_NORETRY)

#define __GFP_NOWARN	((__force gfp_t)___GFP_NOWARN)
#define __GFP_ZERO	((__force gfp_t)___GFP_ZERO)

#define __GFP_BITS_SHIFT 25	/* Room for N __GFP_FOO bits */
#define __GFP_BITS_MASK ((__force gfp_t)((1 << __GFP_BITS_SHIFT) - 1))

#define GFP_ATOMIC	(__GFP_HIGH|__GFP_ATOMIC|__GFP_KSWAPD_RECLAIM)
#define GFP_KERNEL	(__GFP_RECLAIM | __GFP_IO | __GFP_FS)
#define GFP_KERNEL_ACCOUNT (GFP_KERNEL | __GFP_ACCOUNT)
#define GFP_NOWAIT	(__GFP_KSWAPD_RECLAIM)
#define GFP_NOIO	(__GFP_RECLAIM)
#define GFP_NOFS	(__GFP_RECLAIM | __GFP_IO)
#define GFP_USER	(__GFP_RECLAIM | __GFP_IO | __GFP_FS | __GFP_HARDWALL)
#define GFP_DMA		__GFP_DMA
#define GFP_DMA32	__GFP_DMA32
#define GFP_HIGHUSER	(GFP_USER | __GFP_HIGHMEM)
#define GFP_HIGHUSER_MOVABLE	(GFP_HIGHUSER | __GFP_MOVABLE)

struct page {
	void *address;
	unsigned long private;
};

#ifdef SMALL_PAGE_SIZE_DEBUG
#define PAGE_SIZE	(1UL << 6)
#else
#define PAGE_SIZE	(1UL << 12)
#endif

static inline void *page_address(struct page *page)
{
	return page->address;
}

static inline struct page *alloc_pages(gfp_t gfp_mask, unsigned order)
{
	struct page *page = malloc(sizeof(*page));
	void *data = malloc(PAGE_SIZE);
	if (!page || !data)
		goto error;
	*page = (struct page){ .address = data };
	return page;

error:
	if (page)
		free(page);
	if (data)
		free(data);
	return NULL;
}
#define alloc_page(gfp_mask) alloc_pages(gfp_mask, 0)

static inline void __free_pages(struct page *page, unsigned order)
{
	free(page_address(page));
	free(page);
}
#define __free_page(page) __free_pages((page), 0)

#ifdef TUX3_BUILD
/* depending on tux3 */

#include <tux3user.h>

/*
 * pagemap stuff
 */

static inline gfp_t mapping_gfp_mask(map_t * mapping)
{
	return (__force gfp_t)mapping->flags & __GFP_BITS_MASK;
}

/*
 * This is non-atomic.  Only to be used before the mapping is activated.
 * Probably needs a barrier...
 */
static inline void mapping_set_gfp_mask(map_t *m, gfp_t mask)
{
	m->flags = (m->flags & ~(__force unsigned long)__GFP_BITS_MASK) |
				(__force unsigned long)mask;
}

/*
 * mm stuff
 */

static inline void truncate_inode_pages_range(map_t *map, loff_t lstart, loff_t lend)
{
	truncate_buffers_range(map, lstart, lend);
}

static inline void truncate_inode_pages(map_t *map, loff_t lstart)
{
	truncate_buffers_range(map, lstart, LLONG_MAX);
}

static inline void truncate_inode_pages_final(map_t *map)
{
	truncate_inode_pages(map, 0);
}

void truncate_pagecache(struct inode *inode, loff_t newsize);
void truncate_setsize(struct inode *inode, loff_t newsize);
static inline void pagecache_isize_extended(struct inode *inode, loff_t from,
					    loff_t to)
{
	/* Nothing need to do. */
}
#endif /* TUX3_BUILD */

#endif /* !LIBKLIB_MM_H */
