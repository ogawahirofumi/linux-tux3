#ifndef LIBKLIB_MM_H
#define LIBKLIB_MM_H

/* depending on tux3 */

/*
 * gfp stuff
 */

/* Plain integer GFP bitmasks. Do not use this directly. */
#define ___GFP_HIGHMEM		0x02u
#define ___GFP_MOVABLE		0x08u
#define ___GFP_WAIT		0x10u
#define ___GFP_HIGH		0x20u
#define ___GFP_IO		0x40u
#define ___GFP_FS		0x80u
#define ___GFP_NOFAIL		0x800u
#define ___GFP_ZERO		0x8000u
#define ___GFP_HARDWALL		0x20000u

#define __GFP_HIGHMEM	((__force gfp_t)___GFP_HIGHMEM)
#define __GFP_MOVABLE	((__force gfp_t)___GFP_MOVABLE)  /* Page is movable */

#define __GFP_WAIT	((__force gfp_t)___GFP_WAIT)
#define __GFP_HIGH	((__force gfp_t)___GFP_HIGH)
#define __GFP_IO	((__force gfp_t)___GFP_IO)
#define __GFP_FS	((__force gfp_t)___GFP_FS)
#define __GFP_REPEAT	((__force gfp_t)___GFP_REPEAT)
#define __GFP_NOFAIL	((__force gfp_t)___GFP_NOFAIL)
#define __GFP_NORETRY	((__force gfp_t)___GFP_NORETRY)
#define __GFP_ZERO	((__force gfp_t)___GFP_ZERO)
#define __GFP_HARDWALL	((__force gfp_t)___GFP_HARDWALL)

#define __GFP_BITS_SHIFT 25	/* Room for N __GFP_FOO bits */
#define __GFP_BITS_MASK ((__force gfp_t)((1 << __GFP_BITS_SHIFT) - 1))

/* This equals 0, but use constants in case they ever change */
#define GFP_NOWAIT	(GFP_ATOMIC & ~__GFP_HIGH)
/* GFP_ATOMIC means both !wait (__GFP_WAIT not set) and use emergency pool */
#define GFP_ATOMIC	(__GFP_HIGH)
#define GFP_NOIO	(__GFP_WAIT)
#define GFP_NOFS	(__GFP_WAIT | __GFP_IO)
#define GFP_KERNEL	(__GFP_WAIT | __GFP_IO | __GFP_FS)
#define GFP_USER	(__GFP_WAIT | __GFP_IO | __GFP_FS | __GFP_HARDWALL)
#define GFP_HIGHUSER	(GFP_USER | __GFP_HIGHMEM)
#define GFP_HIGHUSER_MOVABLE	(GFP_HIGHUSER | __GFP_MOVABLE)

struct page {
	void *address;
	unsigned long private;
};

#define PAGE_SIZE	(1 << 6)

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

/*
 * pagemap stuff
 */

#define PAGE_CACHE_SIZE	PAGE_SIZE

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

#endif /* !LIBKLIB_MM_H */
