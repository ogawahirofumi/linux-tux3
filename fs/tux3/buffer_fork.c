/*
 * Block Fork (Copy-On-Write of logically addressed block).
 *
 * Copyright (c) 2008-2014 OGAWA Hirofumi
 */

#include <linux/hugetlb.h>	/* for PageHuge() */
#include <linux/swap.h>		/* for lru_cache_add_file() */
#include <linux/cleancache.h>

/*
 * Scanning the freeable forked page.
 *
 * Although we would like to free forked page at early stage (e.g. in
 * blockdirty()). To free page, we have to set NULL to page->mapping,
 * and free buffers on the page. But reader side can be grabbing the
 * forked page, and may use ->mapping or buffers.  So, we have to
 * keep forked page as is until it can be freed.
 *
 * So, we check the forked pages periodically. And if all referencer
 * are gone (checking page_count()), free forked buffer and page.
 */

#define buffer_link(x)		((struct link *)&(x)->b_end_io)
#define buffer_link_entry(x)	__link_entry(x, struct buffer_head, b_end_io)

/*
 * Register forked buffer to free the page later.
 * FIXME: we should replace the hack link by ->b_end_io with something
 */
static void forked_buffer_add(struct sb *sb, struct buffer_head *buffer)
{
	/* Pin buffer. This prevents try_to_free_buffers(). */
	get_bh(buffer);

	spin_lock(&sb->forked_buffers_lock);
	link_add(buffer_link(buffer), &sb->forked_buffers);
	spin_unlock(&sb->forked_buffers_lock);
}

static void forked_buffer_del(struct link *prev, struct buffer_head *buffer)
{
	link_del_next(prev);
	/* Unpin buffer */
	put_bh(buffer);
}

/* Cleaning and free forked page */
static void free_forked_page(struct page *page)
{
	struct address_space *mapping = page->mapping;

	assert(PageForked(page));

	lock_page(page);
	if (page_has_buffers(page)) {
		int ret = try_to_free_buffers(page);
		assert(ret);
	}
	/* Lock is to make sure end_page_writeback() was done completely */
	xa_lock_irq(&mapping->i_pages);
	page->mapping = NULL;
	xa_unlock_irq(&mapping->i_pages);
	unlock_page(page);

	/* Drop the radix-tree reference */
	put_page(page);
	/* Drop the final reference */
	trace_on("page %p, count %u", page, page_count(page));
	put_page(page);
}

/* Use same bit with bufdelta though, this buffer never be dirty */
#define buffer_freeable(x)	test_bit(BH_PrivateStart, &(x)->b_state)
#define set_buffer_freeable(x)	set_bit(BH_PrivateStart, &(x)->b_state)
#define clear_buffer_freeable(x) clear_bit(BH_PrivateStart, &(x)->b_state)

static inline int buffer_busy(struct buffer_head *buffer, int refcount)
{
	/*
	 * Page didn't have dirty and writeback, so this buffer should
	 * already be flushed. Check if reader is still using this.
	 */
	assert(!buffer_dirty(buffer));
	assert(!buffer_async_write(buffer));
	assert(!buffer_async_read(buffer));

	return atomic_read(&buffer->b_count) > refcount ||
		buffer_locked(buffer);
}

/* There is no referencer? */
static int is_freeable_forked(struct buffer_head *buffer, struct page *page)
{
	/*
	 * There is no reference of buffers? Once reader released
	 * buffer, it never grab again. So we don't need recheck it.
	 */
	if (!buffer_freeable(buffer)) {
		struct buffer_head *tmp = buffer->b_this_page;
		while (tmp != buffer) {
			if (buffer_busy(tmp, 0))
				return 0;
			tmp = tmp->b_this_page;
		}
		/* we have the refcount of this buffer to pin */
		if (buffer_busy(buffer, 1))
			return 0;

		set_buffer_freeable(buffer);
	}

	/* Page is freeable? (radix-tree + ->private + own) */
	return page_count(page) == 3;
}

/*
 * Try to free forked page. (If it is called from umount or evict_inode
 * path, there should be no referencer. So we free forked page
 * forcefully.)
 *
 * inode: Free only if page is related to this inode.
 * force: If true, even if refcount != 0 try to free.
 *
 * FIXME: we need the better way, instead of polling the freeable
 * forked pages periodically.
 */
void free_forked_buffers(struct sb *sb, struct inode *inode, int force)
{
	struct link free_list, *node, *prev, *n;

	init_link_circular(&free_list);

	/* Move freeable forked page to free_list */
	spin_lock(&sb->forked_buffers_lock);
	link_for_each_safe(node, prev, n, &sb->forked_buffers) {
		struct buffer_head *buffer = buffer_link_entry(node);
		struct page *page = buffer->b_page;

		trace_on("buffer %p, page %p, count %u",
			 buffer, page, page_count(page));

		if (inode) {
			/* Free only if page is related to inode */
			if (page->mapping != inode->i_mapping)
				continue;
		}

#ifdef TUX3_FLUSHER_SYNC
		/* The page should already be submitted if no async frontend */
		assert(!PageDirty(page));
#endif
		assert(!force || (!PageDirty(page) && !PageWriteback(page)));

		/*
		 * I/O was submitted and I/O was done?
		 *
		 * NOTE: order of checking flags is important.
		 *
		 * free_forked_buffers	    bufvec_prepare_and_lock_page
		 *	PageWriteback()
		 *				TestSetPageWriteback()
		 *				TestClearPageDirty()
		 *	PageDirty()
		 *	[missed both flags]
		 *
		 * Above order has race. So, we have to check "dirty"
		 * at first, then check "writeback".
		 *
		 * FIXME: we would not want to depend on this fragile
		 * way, and would want to use refcount simply to free
		 * forked page.
		 */
		if (!PageDirty(page) && !PageWriteback(page)) {
			/* All users were gone or force=1? */
			if (force || is_freeable_forked(buffer, page)) {
				clear_buffer_freeable(buffer);

				link_del_next(prev);
				link_add(buffer_link(buffer), &free_list);
			}
		}
	}
	spin_unlock(&sb->forked_buffers_lock);

	/* Free forked pages */
	while (!link_empty(&free_list)) {
		struct buffer_head *buffer = buffer_link_entry(free_list.next);
		struct page *page = buffer->b_page;

		forked_buffer_del(&free_list, buffer);
		free_forked_page(page);
	}
}

/*
 * Block fork core
 */

/*
 * Clear writable to protect oldpage from following mmap write race.
 *
 *        cpu0                          cpu1                   cpu2
 *                                                           [mmap write]
 *                                                           mmap write(old)
 *                                                               page fault
 *                                     [backend]                 dirty old
 *                                     delta++
 *    [page_fault]
 *    page fork
 *                                                           mmap write(old)
 *                                                               no page fault
 *        copy_page(new, old)                                    modify page
 *        replace_pte(new, old)
 *                                     flusher
 *                                     page_mkclean(old)
 *
 * There is delay between delta++ and page_mkclean() for I/O. So,
 * while cpu0 copying data on page by page fork, another cpu (cpu2)
 * can change data on the same page. If this race happens, new and old
 * page can have different data.
 */
static void prepare_clone_page(struct page *page)
{
	assert(PageLocked(page));

	/*
	 * If backend flusher is still not clearing the dirty flag and
	 * (not call page_mkclean()) for I/O. Call it here to prevent
	 * above race, instead.
	 */
	if (PageDirty(page))
		page_mkclean(page);
}

/*
 * Clone buffers. But cloned buffer represents the buffer state after
 * flushing buffer.
 */
static void clone_buffers(struct page *oldpage, struct page *newpage)
{
	struct sb *sb = tux_sb(oldpage->mapping->host->i_sb);
	struct buffer_head *head, *newbuf, *oldbuf;
#if 1	/* For now, writeback doesn't use BH_Lock */
#define USE_FOR_IO					\
	((1UL << BH_Uptodate_Lock) | (1UL << BH_Async_Write))
#else
#define USE_FOR_IO					\
	((1UL << BH_Lock) | (1UL << BH_Uptodate_Lock) | (1UL << BH_Async_Write))
#endif

	oldbuf = page_buffers(oldpage);
	newbuf = page_buffers(newpage);
	head = newbuf;
	do {
		assert(!buffer_locked(oldbuf));
		assert(!buffer_async_read(oldbuf));

		newbuf->b_state = oldbuf->b_state;
		/* Adjust ->b_state to after I/O */
		newbuf->b_state &= ~USE_FOR_IO;
		if (buffer_dirty(newbuf))
			tux3_clear_buffer_dirty_for_io(newbuf, sb, 0);

		oldbuf = oldbuf->b_this_page;
		newbuf = newbuf->b_this_page;
	} while (newbuf != head);
}

/* Copy newpage from oldpage for page forking. */
static struct page *tux3_clone_page(struct page *oldpage, unsigned blocksize)
{
	struct page *newpage;

	/* oldpage should be forked page */
	BUG_ON(PageForked(oldpage));

	newpage = pagefork_clone_page(oldpage);
	if (!IS_ERR(newpage)) {
		create_empty_buffers(newpage, blocksize, 0);
		clone_buffers(oldpage, newpage);
	}

	return newpage;
}

/* Try to remove from LRU list */
static void oldpage_try_remove_from_lru(struct page *page)
{
	/* Required functions are not exported at 3.4.4 */
}

/* Schedule to add LRU list (based on putback_lru_page()) */
static void newpage_add_lru(struct page *page)
{
	/*
	 * FIXME: we want to back same LRU type with oldpage, but
	 * lru_cache_add() is not exported. (lru_cache_add_file()
	 * calls ClearPageActive())
	 *
	 * So, this try to make page active by mark_page_accessed().
	 */
	int active = PageActive(page);
	int referenced = PageReferenced(page);

	lru_cache_add_file(page);

	if (active) {
		/* Make active,unreferenced */
		if (!referenced)
			SetPageReferenced(page);
		mark_page_accessed(page);
		/* Make active,referenced */
		if (referenced)
			SetPageReferenced(page);
	}
}

enum ret_needfork {
	RET_FORKED = 1,		/* Someone already forked */
	RET_NEED_FORK,		/* Need to fork to dirty */
	RET_CAN_DIRTY,		/* Can dirty without fork */
	RET_ALREADY_DIRTY,	/* Buffer is already dirtied for delta */
};

static enum ret_needfork
need_fork(struct page *page, struct buffer_head *buffer, unsigned delta)
{
	struct buffer_head *tmp;
	int bufdelta;

	/* Someone already forked this page. */
	if (PageForked(page))
		return RET_FORKED;
	/* Page is under I/O, needs buffer fork */
	if (PageWriteback(page))
		return RET_NEED_FORK;
	/*
	 * If page isn't dirty (and isn't writeback), this is clean
	 * page (and all buffers should be clean on this page).  So we
	 * can just dirty the buffer for current delta.
	 */
	if (!PageDirty(page)) {
		assert(!buffer || !buffer_dirty(buffer));
		return RET_CAN_DIRTY;
	}
	if (buffer == NULL) {
		/* If the page is dirty, it should have buffers */
		assert(page_has_buffers(page));
		buffer = page_buffers(page);
	}

	/*
	 * (Re-)check the buffer and page under lock_page. (We don't
	 * allow the buffer has different delta states on same page.)
	 */
	bufdelta = buffer_check_dirty_delta(buffer->b_state);
	if (bufdelta >= 0) {
		/* Buffer is dirtied by delta, just modify this buffer */
		if (bufdelta == tux3_delta(delta))
			return RET_ALREADY_DIRTY;

		/* Buffer was dirtied by different delta, we need buffer fork */
		return RET_NEED_FORK;
	}

	/*
	 * Check other buffers sharing same page.
	 */
	tmp = buffer->b_this_page;
	while (tmp != buffer) {
		if (!buffer_can_modify(tmp, delta)) {
			/* The buffer can't be modified for delta */
			return RET_NEED_FORK;
		}

		tmp = tmp->b_this_page;
	}

	/* This page can be modified, dirty this buffer */
	return RET_CAN_DIRTY;
}

/*
 * Clone page, then replace slot by newpage in radix-tree
 *
 * need_unmap: need to unmap from PTE.
 * keep_refcnt: keep refcount of oldpage after fork.
 */
static struct page *
tux3_fork_page(struct vm_area_struct *vma, bool need_unmap,
	       struct page *oldpage, bool keep_refcnt)
{
	/* Checked buffer and oldpage, now oldpage->mapping should be valid. */
	struct sb *sb = tux_sb(oldpage->mapping->host->i_sb);
	struct page *newpage;
	int err;

	if (need_unmap) {
		/* Clear writable to protect oldpage from mmap write race */
		prepare_clone_page(oldpage);
	}

	/*
	 * We need to buffer fork. Start to clone the oldpage.
	 */
	newpage = tux3_clone_page(oldpage, sb->blocksize);
	if (IS_ERR(newpage))
		return newpage;

	/*
	 * We keep page->mapping as is for writeback. If keep_refcnt==true,
	 * keep refcount of oldpage. If not, inherit refcount of caller
	 * for radix-tree.
	 */
	if (keep_refcnt)
		get_page(oldpage);

	/* Replace oldpage on radix-tree with newpage */
	err = pagefork_replace_page_cache(oldpage, newpage); /* FIXME: error */

	newpage_add_lru(newpage);

	/*
	 * Referencer are dummy radix-tree + ->private (plus other
	 * users and lru_cache).
	 *
	 * FIXME: We can't remove from LRU, because page can be on
	 * per-cpu lru cache at here. So, vmscan will try to free
	 * oldpage. We get refcount to pin oldpage to prevent vmscan
	 * try to release oldpage.
	 */
	trace("oldpage count %u", page_count(oldpage));
	assert(page_count(oldpage) >= 2);
	get_page(oldpage);
	oldpage_try_remove_from_lru(oldpage);

	/*
	 * This prevents to re-fork the oldpage. And we guarantee the
	 * newpage is available on radix-tree here.
	 */
	SetPageForked(oldpage);
	if (need_unmap) {
		/* Update PTEs for forked page. */
		page_pagefork_file(vma, oldpage, newpage);
	}
	unlock_page(oldpage);

	/* Register forked buffer to free forked page later */
	forked_buffer_add(sb, page_buffers(oldpage));

	return newpage;
}

struct buffer_head *blockdirty(struct buffer_head *buffer, unsigned newdelta)
{
	struct page *newpage, *oldpage = buffer->b_page;
	struct sb *sb;
	struct buffer_head *newbuf;
	enum ret_needfork ret_needfork;

	trace("buffer %p, page %p, index %lx, count %u",
	      buffer, oldpage, oldpage->index, page_count(oldpage));
	trace("forked %u, dirty %u, writeback %u",
	      PageForked(oldpage), PageDirty(oldpage), PageWriteback(oldpage));

	/* The simple case: redirty on same delta */
	if (buffer_already_dirty(buffer, newdelta))
		return buffer;

	/* Take page lock to protect buffer list, and concurrent block_fork */
	lock_page(oldpage);

	/* This happens on partially dirty page. */
//	assert(PageUptodate(oldpage));
	assert(!page_mapped(oldpage));

	switch ((ret_needfork = need_fork(oldpage, buffer, newdelta))) {
	case RET_FORKED:
		/* This page was already forked. Retry from lookup page. */
		buffer = ERR_PTR(-EAGAIN);
		WARN_ON(1);
		/* FALLTHRU */
	case RET_ALREADY_DIRTY:
		/* This buffer was already dirtied. Done. */
		goto out;
	case RET_CAN_DIRTY:
	case RET_NEED_FORK:
		break;
	default:
		BUG();
		break;
	}

	if (ret_needfork == RET_CAN_DIRTY) {
		/* We can dirty this buffer. */
		goto dirty_buffer;
	}

	/* Checked buffer and oldpage, now oldpage->mapping should be valid. */
	sb = tux_sb(oldpage->mapping->host->i_sb);

	newpage = tux3_fork_page(NULL, false, oldpage, true);
	if (IS_ERR(newpage)) {
		buffer = ERR_CAST(newpage);
		goto out;
	}

	newbuf = __get_buffer(newpage, bh_offset(buffer) >> sb->blockbits);
	/* Grab buffer to pin page, then release refcount of newpage */
	get_bh(newbuf);
	put_page(newpage);

	/* Release buffer (so unpin oldpage too). */
	brelse(buffer);

	trace("cloned page %p, buffer %p", newpage, newbuf);
	buffer = newbuf;
	oldpage = newpage;

dirty_buffer:
	assert(!buffer_dirty(buffer));
	__tux3_mark_buffer_dirty(buffer, newdelta);

out:
	unlock_page(oldpage);

	return buffer;
}

/*
 * Do buffer fork for oldpage if needed. Then return page with locked.
 * Page is locked, so, the caller can call __tux3_mark_buffer_dirty()
 * (without checking buffer fork) to dirty buffers on the returned page,
 * until unlock page.
 *
 * Caller must hold refcount of oldpage and hold lock_page(oldpage)
 */
struct page *pagefork_for_blockdirty(struct vm_area_struct *vma,
				     struct page *oldpage, bool keep_refcnt,
				     unsigned newdelta)
{
	struct page *newpage = oldpage;
	enum ret_needfork ret_needfork;

	/* Check page lock to protect buffer list, and concurrent block_fork */
	assert(PageLocked(oldpage));

	trace("page %p, index %lx, count %u",
	      oldpage, oldpage->index, page_count(oldpage));
	trace("forked %u, dirty %u, writeback %u",
	      PageForked(oldpage), PageDirty(oldpage), PageWriteback(oldpage));

	/* This happens on partially dirty page. */
//	assert(PageUptodate(page));

	switch ((ret_needfork = need_fork(oldpage, NULL, newdelta))) {
	case RET_FORKED:
		/* This page was already forked. Retry from lookup page. */
		newpage = ERR_PTR(-EAGAIN);
		WARN_ON(vma == NULL);
	case RET_ALREADY_DIRTY:
		/* This buffer was already dirtied. Done. */
		goto out;
	case RET_CAN_DIRTY:
	case RET_NEED_FORK:
		break;
	default:
		BUG();
		break;
	}

	if (ret_needfork == RET_CAN_DIRTY) {
		/* We can dirty this buffer. */
		goto out;
	}

	newpage = tux3_fork_page(vma, true, oldpage, keep_refcnt);
out:
	return newpage;
}

/*
 * This checks the page whether we can invalidate. If the page is
 * stabled, we can't invalidate the buffers on page. So, this forks
 * the page without making clone page.
 *
 * 1 - fork was done to invalidate (i.e. page was removed from radix-tree)
 * 0 - fork was not done (i.e. buffers on page can be invalidated)
 */
int bufferfork_to_invalidate(struct address_space *mapping, struct page *page)
{
	struct sb *sb = tux_sb(mapping->host->i_sb);
	unsigned delta = tux3_inode_delta(mapping->host);

	assert(PageLocked(page));
	assert(!page_mapped(page));

	switch (need_fork(page, NULL, delta)) {
	case RET_NEED_FORK:
		/* Need to fork, then delete from radix-tree */
		break;
	case RET_ALREADY_DIRTY:
	case RET_CAN_DIRTY:
		/* We can invalidate the page */
		return 0;
	case RET_FORKED:
		trace_on("mapping %p, page %p", mapping, page);
		/* FALLTHRU */
	default:
		BUG();
		break;
	}

	/* We keep page->mapping as is, so get refcount for radix-tree. */
	get_page(page);

	/* FIXME: need this? */
	ClearPageMappedToDisk(page);
	/* Delete page from radix-tree */
	pagefork_delete_from_page_cache(page);

	/*
	 * Referencer are dummy radix-tree + ->private (plus other
	 * users and lru_cache).
	 *
	 * FIXME: We can't remove from LRU, because page can be on
	 * per-cpu lru cache at here. So, vmscan will try to free
	 * page. We get refcount to pin page to prevent vmscan
	 * try to release page.
	 */
	trace("page count %u", page_count(page));
	assert(page_count(page) >= 2);
	get_page(page);
	oldpage_try_remove_from_lru(page);

	/*
	 * This prevents to re-fork the page. And we guarantee the
	 * newpage is available on radix-tree here.
	 */
	SetPageForked(page);

	/* Register forked buffer to free forked page later */
	forked_buffer_add(sb, page_buffers(page));

	return 1;
}
