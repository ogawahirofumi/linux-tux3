/*
 * mmap(2) handlers to support page fork.
 *
 * Copyright (c) 2008-2014 OGAWA Hirofumi
 */

static int tux3_set_page_dirty_buffers(struct page *page)
{
#if 0
	int newly_dirty;
	struct address_space *mapping = page_mapping(page);

	if (unlikely(!mapping))
		return !TestSetPageDirty(page);

	spin_lock(&mapping->private_lock);
	if (page_has_buffers(page)) {
		struct buffer_head *head = page_buffers(page);
		struct buffer_head *bh = head;

		do {
			set_buffer_dirty(bh);
			bh = bh->b_this_page;
		} while (bh != head);
	}
	/*
	 * Lock out page->mem_cgroup migration to keep PageDirty
	 * synchronized with per-memcg dirty page counters.
	 */
	lock_page_memcg(page);
	newly_dirty = !TestSetPageDirty(page);
	spin_unlock(&mapping->private_lock);

	if (newly_dirty)
		__set_page_dirty(page, mapping, 1);

	unlock_page_memcg(page);

	if (newly_dirty)
		__mark_inode_dirty(mapping->host, I_DIRTY_PAGES);

	return newly_dirty;
#else
	int newly_dirty;
	struct address_space *mapping = page_mapping(page);
	unsigned delta = tux3_get_current_delta();
	struct buffer_head *head, *buffer;

	/* This should be tux3 page and locked */
	assert(mapping);
	assert(PageLocked(page));
	/* This page should have buffers (caller should allocate) */
	assert(page_has_buffers(page));

	/*
	 * FIXME: we dirty all buffers on this page, so we optimize this
	 * by avoiding to check page-dirty/inode-dirty multiple times.
	 */

	/*
	 * Lock out page->mem_cgroup migration to keep PageDirty
	 * synchronized with per-memcg dirty page counters.
	 */
	lock_page_memcg(page);
	newly_dirty = 0;
	if (!TestSetPageDirty(page)) {
		__set_page_dirty(page, mapping, 1);
		newly_dirty = 1;
	}
	unlock_page_memcg(page);

	buffer = head = page_buffers(page);
	do {
		__tux3_mark_buffer_dirty(buffer, delta);
		buffer = buffer->b_this_page;
	} while (buffer != head);

	if (newly_dirty)
		__tux3_mark_inode_dirty(mapping->host, I_DIRTY_PAGES);
#endif
	return newly_dirty;
}

/* Copy of set_page_dirty() */
static int tux3_set_page_dirty(struct page *page)
{
	/*
	 * readahead/lru_deactivate_page could remain
	 * PG_readahead/PG_reclaim due to race with end_page_writeback
	 * About readahead, if the page is written, the flags would be
	 * reset. So no problem.
	 * About lru_deactivate_page, if the page is redirty, the flag
	 * will be reset. So no problem. but if the page is used by readahead
	 * it will confuse readahead and make it restart the size rampup
	 * process. But it's a trivial problem.
	 */
	if (PageReclaim(page))
		ClearPageReclaim(page);

	return tux3_set_page_dirty_buffers(page);
}

static int tux3_set_page_dirty_assert(struct page *page)
{
	struct buffer_head *head, *buffer;

	/* See comment of tux3_set_page_dirty() */
	if (PageReclaim(page))
		ClearPageReclaim(page);

	/* Is there any cases to be called for old page of forked page? */
	WARN_ON(PageForked(page));

	/* This page should be dirty already, otherwise we will lost data. */
	assert(PageDirty(page));
	/* All buffers should be dirty already, otherwise we will lost data. */
	assert(page_has_buffers(page));
	head = buffer = page_buffers(page);
	do {
		assert(buffer_dirty(buffer));
		buffer = buffer->b_this_page;
	} while (buffer != head);

	return 0;
}

static int tux3_set_page_dirty_bug(struct page *page)
{
	/* See comment of tux3_set_page_dirty() */
	if (PageReclaim(page))
		ClearPageReclaim(page);

	assert(0);
	/* This page should not be mmapped */
	assert(!page_mapped(page));
	/* This page should be dirty already, otherwise we will lost data. */
	assert(PageDirty(page));
	return 0;
}

/*
 * NOTE: This keeps refcount of original vmf->page, refcount is
 * released by caller.
 */
static vm_fault_t tux3_page_mkwrite(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct inode *inode = file_inode(vma->vm_file);
	struct sb *sb = tux_sb(inode->i_sb);
	struct page *clone, *page = vmf->page;
	unsigned delta;
	int cost;
	vm_fault_t ret;

	sb_start_pagefault(inode->i_sb);
	down_read(&tux_inode(inode)->truncate_lock);

retry:
	lock_page(page);
	if (page->mapping != mapping(inode)) {
		unlock_page(page);
		ret = VM_FAULT_NOPAGE;
		goto error;
	}

	/*
	 * FIXME: If page fault happened while holding change_begin/end()
	 * (e.g. copy of user data between ->write_begin and ->write_end
	 * for write(2)), this doesn't work. Because this context may hold
	 * old delta which wants to flush for ENOSPC check.
	 */
	cost = nospc_one_page_cost(inode);
	while (change_begin_nospc(sb, cost, 0)) {
		unlock_page(page);
		if (nospc_wait_and_check(sb, cost, 0)) {
			ret = VM_FAULT_SIGBUS; /* -ENOSPC */
			goto error;
		}

		lock_page(page);
		/*
		 * Since holding ->truncate_lock, so no need to
		 * recheck page->mapping.
		 *
		 * 	if (page->mapping != mapping(inode))
		 */
		BUG_ON(page->mapping != mapping(inode));
	}

	delta = tux3_get_current_delta();
	clone = pagefork_for_blockdirty(vma, page, page == vmf->page, delta);
	if (IS_ERR(clone)) {
		int err = PTR_ERR(clone);

		change_end(sb);
		unlock_page(page);

		if (err == -EAGAIN) {
			pgoff_t index = page->index;

			/* Don't touch refcount if old page */
			if (page != vmf->page)
				put_page(page);

			/* Someone did page fork */
			page = find_get_page(inode->i_mapping, index);
			assert(page);
			goto retry;
		}

		/* Error happened */
		ret = block_page_mkwrite_return(err);
		goto error;
	}

	file_update_time(vma->vm_file);

	/* Assign buffers to dirty */
	if (!page_has_buffers(clone))
		create_empty_buffers(clone, sb->blocksize, 0);

	/*
	 * We mark the page dirty already here so that when freeze is in
	 * progress, we are guaranteed that writeback during freezing will
	 * see the dirty page and writeprotect it again.
	 */
	tux3_set_page_dirty(clone);

	change_end(sb);
	/* If page was forked, remember oldpage */
	if (vmf->page != clone)
		vmf->forked_oldpage = vmf->page;
	vmf->page = clone;
	ret = VM_FAULT_LOCKED;

out:
	up_read(&tux_inode(inode)->truncate_lock);
	sb_end_pagefault(inode->i_sb);

	return ret;

error:
	/* Don't touch refcount if orig_page */
	if (page != vmf->page)
		put_page(page);
	goto out;
}

static const struct vm_operations_struct tux3_file_vm_ops = {
	.fault		= filemap_fault,
	.map_pages	= filemap_map_pages,
	.page_mkwrite	= tux3_page_mkwrite,
};

int tux3_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct address_space *mapping = file->f_mapping;

	if (!mapping->a_ops->readpage)
		return -ENOEXEC;

	file_accessed(file);
	vma->vm_ops = &tux3_file_vm_ops;

	return 0;
}
