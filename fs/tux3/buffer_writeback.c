/*
 * Write back block buffers.
 *
 * Copyright (c) 2008-2014 OGAWA Hirofumi
 */

#include "buffer_writebacklib.c"
#include "ioinfo.h"

/*
 * Helper for buffer vector I/O.
 */

static inline struct buffer_head *buffers_entry(struct list_head *x)
{
	return list_entry(x, struct buffer_head, b_assoc_buffers);
}

#define MAX_BUFVEC_COUNT	UINT_MAX

/* Initialize bufvec */
static void bufvec_init(struct bufvec *bufvec, enum req_opf req_opf,
			unsigned int req_flags, struct address_space *mapping,
			struct list_head *head, struct tux3_iattr_data *idata)
{
	INIT_LIST_HEAD(&bufvec->contig);
	bufvec->req_opf		= req_opf;
	bufvec->req_flags	= req_flags;
	bufvec->buffers		= head;
	bufvec->contig_count	= 0;
	bufvec->idata		= idata;
	bufvec->mapping		= mapping;
	bufvec->on_page_idx	= 0;
	bufvec->bio		= NULL;
	bufvec->bio_lastbuf	= NULL;
}

static void bufvec_free(struct bufvec *bufvec)
{
	/* FIXME: on error path, this will happens */
	assert(!bufvec->buffers || list_empty(bufvec->buffers));
	assert(list_empty(&bufvec->contig));
	assert(bufvec->bio == NULL);
}

static inline void bufvec_buffer_move_to_contig(struct bufvec *bufvec,
						struct buffer_head *buffer)
{
	/*
	 * This is called by backend, it means buffer state should be
	 * stable. So, we don't need lock for buffer state list
	 * (->b_assoc_buffers).
	 *
	 * FIXME: above is true?
	 */
	list_move_tail(&buffer->b_assoc_buffers, &bufvec->contig);
	bufvec->contig_count++;
}

/*
 * Special purpose single pointer list (FIFO order) for buffers on bio
 */
static void bufvec_bio_add_buffer(struct bufvec *bufvec,
				  struct buffer_head *new)
{
	new->b_private = NULL;

	if (bufvec->bio_lastbuf)
		bufvec->bio_lastbuf->b_private = new;
	else
		bufvec->bio->bi_private = new;

	bufvec->bio_lastbuf = new;
}

static struct buffer_head *bufvec_bio_del_buffer(struct bio *bio)
{
	struct buffer_head *buffer = bio->bi_private;

	if (buffer) {
		bio->bi_private = buffer->b_private;
		buffer->b_private = NULL;
	}

	return buffer;
}

static struct address_space *bufvec_bio_mapping(struct bio *bio)
{
	struct buffer_head *buffer = bio->bi_private;
	assert(buffer);
	/* FIXME: we want to remove usage of b_assoc_map */
	return buffer->b_assoc_map;
}

static struct bio *bufvec_bio_alloc(struct sb *sb, unsigned int count,
				    block_t physical,
				    bio_end_io_t *end_io)
{
	gfp_t gfp_flags = GFP_NOFS;
	struct bio *bio;

	count = bio_max_segs(count);

	bio = bio_alloc(gfp_flags, count);
	/* This retry is from mpage_alloc() */
	if (bio == NULL && (current->flags & PF_MEMALLOC)) {
		while (!bio && (count /= 2))
			bio = bio_alloc(gfp_flags, count);
	}
	assert(bio);	/* GFP_NOFS shouldn't fail to allocate */

	bio_set_dev(bio, vfs_sb(sb)->s_bdev);
	bio->bi_iter.bi_sector = physical << (sb->blockbits - 9);
	bio->bi_end_io = end_io;

	return bio;
}

static void bufvec_submit_bio(struct bufvec *bufvec)
{
	struct inode *inode = bufvec_inode(bufvec);
	struct sb *sb = tux_sb(inode->i_sb);
	struct bio *bio = bufvec->bio;

	bufvec->bio = NULL;
	bufvec->bio_lastbuf = NULL;

	trace("bio %p, physical %Lu, count %u", bio,
	      (block_t)bio->bi_iter.bi_sector >> (sb->blockbits - 9),
	      bio->bi_iter.bi_size >> sb->blockbits);

	tux3_io_inflight_inc(sb->ioinfo);
	bio->bi_write_hint = inode->i_write_hint;
	bio_set_op_attrs(bio, bufvec->req_opf, bufvec->req_flags);
	submit_bio(bio);
}

/*
 * We flush all buffers on this page?
 *
 * The page may have the dirty buffer for both of "delta" and
 * "unify", and we may flush only dirty buffers for "delta". So, if
 * the page still has the dirty buffer, we should still keep the page
 * dirty for "unify".
 */
static int keep_page_dirty(struct page *page, int on_page_idx)
{
	struct buffer_head *first = page_buffers(page);
	struct buffer_head *tmp = first;
	unsigned count = 0;
	do {
		if (buffer_dirty(tmp)) {
			count++;
			/* dirty buffers > flushing buffers? */
			if (count > on_page_idx)
				return 1;
		}
		tmp = tmp->b_this_page;
	} while (tmp != first);

	return 0;
}

/* Preparation and lock page for I/O */
static void prepare_and_lock_page(struct page *page, int on_page_idx,
				  loff_t i_size, int is_volmap)
{
	pgoff_t last_index;
	unsigned offset;
	int old_flag, old_writeback;

	lock_page(page);
	assert(PageDirty(page));
	assert(!PageWriteback(page));

	/*
	 * Set "writeback" flag before clearing "dirty" flag, so, page
	 * presents either of "dirty" or "writeback" flag.  With this,
	 * free_forked_buffers() can check page flags without locking
	 * page. See FIXME of forked_buffers().
	 *
	 * And writeback flag prevents vmscan releases page.
	 */
	old_writeback = TestSetPageWriteback(page);
	assert(!old_writeback);

	/*
	 * NOTE: This has the race if there is concurrent mark
	 * dirty. But we shouldn't use concurrent dirty [B] on volmap.
	 *
	 *           [ A ]                        [ B ]
	 * if (!keep_page_dirty())
	 *                                   mark_buffer_dirty()
	 *                                       TestSetPageDirty()
	 *     // this lost dirty of [B]
	 *     clear_dirty_for_io()
	 */
	if (!is_volmap || !keep_page_dirty(page, on_page_idx)) {
		/* FIXME: remove outside hack */
		int outside;
		offset = i_size & (PAGE_SIZE - 1);
		last_index = i_size >> PAGE_SHIFT;
		outside = offset && last_index == page->index;

		old_flag = tux3_clear_page_dirty_for_io(page, outside);
		assert(old_flag);
	}

	/*
	 * This fixes incoherence of page accounting and page tag by
	 * above change of dirty and writeback.
	 *
	 * NOTE: This is assuming to be called after clearing dirty
	 * (See comment of tux3_clear_page_dirty_for_io()).
	 */
	tux3_test_set_page_writeback(page, old_writeback);

	/*
	 * Zero fill the page for mmap outside i_size after clear dirty.
	 *
	 * The page straddles i_size.  It must be zeroed out on each and every
	 * writepage invocation because it may be mmapped.  "A file is mapped
	 * in multiples of the page size.  For a file that is not a multiple of
	 * the  page size, the remaining memory is zeroed when mapped, and
	 * writes to that region are not written out to the file."
	 */
	offset = i_size & (PAGE_SIZE - 1);
	last_index = i_size >> PAGE_SHIFT;
	if (offset && last_index == page->index)
		zero_user_segment(page, offset, PAGE_SIZE);
}

static void prepare_and_unlock_page(struct page *page)
{
	unlock_page(page);
}

/* Completion of page for I/O */
static void bufvec_page_end_io(struct bio *bio, struct page *page)
{
	page_endio(page, op_is_write(bio_op(bio)),
		   blk_status_to_errno(bio->bi_status));
}

static void buffer_io_error(struct buffer_head *bh, const char *msg)
{
	printk_ratelimited(KERN_ERR
			"Buffer I/O error on dev %pg, logical block %llu%s\n",
			bh->b_bdev, (unsigned long long)bh->b_blocknr, msg);
}

/* Completion of buffer for I/O */
static void bufvec_buffer_end_io(struct bio *bio, struct buffer_head *buffer)
{
	if (!bio->bi_status)
		set_buffer_uptodate(buffer);
	else {
		if (!bio_flagged(bio, BIO_QUIET))
			buffer_io_error(buffer, ", lost page write");
		mark_buffer_write_io_error(buffer);
		clear_buffer_uptodate(buffer);
		SetPageError(buffer->b_page);
	}
}

/* Check whether buffers are contiguous or not. */
static int bufvec_is_multiple_ranges(struct bufvec *bufvec)
{
	block_t logical, physical;
	unsigned int i;

	logical = bufindex(bufvec->on_page[0].buffer);
	physical = bufvec->on_page[0].block;
	for (i = 1; i < bufvec->on_page_idx; i++) {
		if (logical + i != bufindex(bufvec->on_page[i].buffer) ||
		    physical + i != bufvec->on_page[i].block) {
			return 1;
		}
	}

	return 0;
}

/*
 * BIO completion for complex case. There are multiple ranges on the
 * page, and those are submitted BIO for each range. So, completion of
 * the page is only if all BIOs are done.
 */
static void bufvec_end_io_multiple(struct bio *bio)
{
	struct address_space *mapping;
	struct page *page;
	struct buffer_head *buffer, *first, *tmp;
	unsigned long flags;

	trace("bio %p, err %d", bio, bio->bi_status);

	/* FIXME: inode is still guaranteed to be available? */
	mapping = bufvec_bio_mapping(bio);

	buffer = bufvec_bio_del_buffer(bio);
	page = buffer->b_page;
	first = page_buffers(page);

	trace("buffer %p", buffer);
	tux3_clear_buffer_dirty_for_io_hack(buffer);
	bufvec_buffer_end_io(bio, buffer);
	put_bh(buffer);

	tux3_io_inflight_dec(tux_sb(mapping->host->i_sb)->ioinfo);

	/* Check buffers on the page. If all was done, clear writeback */
	spin_lock_irqsave(&first->b_uptodate_lock, flags);
	clear_buffer_async_write(buffer);
	tmp = buffer->b_this_page;
	while (tmp != buffer) {
		if (buffer_async_write(tmp))
			goto still_busy;
		tmp = tmp->b_this_page;
	}
	spin_unlock_irqrestore(&first->b_uptodate_lock, flags);

	bufvec_page_end_io(bio, page);
	bio_put(bio);
	return;

still_busy:
	spin_unlock_irqrestore(&first->b_uptodate_lock, flags);
	bio_put(bio);
}

/*
 * This page across on multiple ranges.
 *
 * To handle I/O completion properly, this sets "buffer_async_write"
 * to all buffers, then submits buffers with own bio. And on end_io,
 * we check if "buffer_async_write" of all buffers was cleared.
 *
 * FIXME: Some buffers on the page can be contiguous, we can submit
 * those as one bio if contiguous.
 */
static void bufvec_bio_add_multiple(struct bufvec *bufvec)
{
	/* FIXME: inode is still guaranteed to be available? */
	struct sb *sb = tux_sb(bufvec_inode(bufvec)->i_sb);
	struct tux3_iattr_data *idata = bufvec->idata;
	struct page *page;
	unsigned int i;

	/* If there is bio, submit it */
	if (bufvec->bio)
		bufvec_submit_bio(bufvec);

	page = bufvec->on_page[0].buffer->b_page;

	/* Prepare the page and buffers on the page for I/O */
	prepare_and_lock_page(page, bufvec->on_page_idx, idata->i_size, 0);
	/* Set buffer_async_write to all buffers at first, then submit */
	for (i = 0; i < bufvec->on_page_idx; i++) {
		struct buffer_head *buffer = bufvec->on_page[i].buffer;
		block_t physical = bufvec->on_page[i].block;
		get_bh(buffer);
		tux3_clear_buffer_dirty_for_io(buffer, sb, physical);
		/* Buffer locking order for I/O is lower index to
		 * bigger index. And grouped by inode. FIXME: is this sane? */
		/* lock_buffer(buffer); FIXME: need? */
		set_buffer_async_write(buffer);
	}

	for (i = 0; i < bufvec->on_page_idx; i++) {
		struct buffer_head *buffer = bufvec->on_page[i].buffer;
		block_t physical = bufvec->on_page[i].block;
		unsigned int length = bufsize(buffer);
		unsigned int offset = bh_offset(buffer);

		bufvec->bio = bufvec_bio_alloc(sb, 1, physical,
					       bufvec_end_io_multiple);

		trace("page %p, index %Lu, physical %Lu, length %u, offset %u",
		      page, bufindex(bufvec->on_page[i].buffer), physical,
		      length, offset);
		if (!bio_add_page(bufvec->bio, page, length, offset))
			assert(0);	/* why? */

		bufvec_bio_add_buffer(bufvec, buffer);

		bufvec_submit_bio(bufvec);
	}
	prepare_and_unlock_page(page);

	bufvec->on_page_idx = 0;
}

/*
 * bio completion for bufvec based I/O
 */
static void bufvec_end_io(struct bio *bio)
{
	struct address_space *mapping;
	struct page *page, *last_page;

	trace("bio %p, err %d", bio, bio->bi_status);

	/* FIXME: inode is still guaranteed to be available? */
	mapping = bufvec_bio_mapping(bio);

	/* Remove buffer from bio, then unlock buffer */
	last_page = NULL;
	while (1) {
		struct buffer_head *buffer = bufvec_bio_del_buffer(bio);
		if (!buffer)
			break;

		page = buffer->b_page;

		trace("buffer %p", buffer);
		tux3_clear_buffer_dirty_for_io_hack(buffer);
		put_bh(buffer);

		if (page != last_page) {
			bufvec_page_end_io(bio, page);
			last_page = page;
		}
	}

	tux3_io_inflight_dec(tux_sb(mapping->host->i_sb)->ioinfo);
	bio_put(bio);
}

/*
 * Try to add buffers on a page to bio. If it was failed, we submit
 * bio, then add buffers on new bio.
 *
 * FIXME: We can free buffers early, and avoid to use buffers in I/O
 * completion, after prepared the page (like __mpage_writepage).
 */
static void bufvec_bio_add_page(struct bufvec *bufvec)
{
	/* FIXME: inode is still guaranteed to be available? */
	struct sb *sb = tux_sb(bufvec_inode(bufvec)->i_sb);
	struct tux3_iattr_data *idata = bufvec->idata;
	struct page *page;
	block_t physical;
	unsigned int i, length, offset;

	page = bufvec->on_page[0].buffer->b_page;
	physical = bufvec->on_page[0].block;
	offset = bh_offset(bufvec->on_page[0].buffer);
	length = bufvec->on_page_idx << sb->blockbits;

	trace("page %p, index %Lu, physical %Lu, length %u, offset %u",
	      page, bufindex(bufvec->on_page[0].buffer), physical,
	      length, offset);

	/* Try to add buffers to exists bio */
	if (!bufvec->bio || !bio_add_page(bufvec->bio, page, length, offset)) {
		/* Couldn't add. So submit old bio and allocate new bio */
		if (bufvec->bio)
			bufvec_submit_bio(bufvec);

		bufvec->bio =
			bufvec_bio_alloc(sb, bufvec_contig_count(bufvec) + 1,
					 physical, bufvec_end_io);

		if (!bio_add_page(bufvec->bio, page, length, offset))
			assert(0);	/* why? */
	}

	/* Prepare the page, and buffers on the page for I/O */
	prepare_and_lock_page(page, bufvec->on_page_idx, idata->i_size, 0);
	for (i = 0; i < bufvec->on_page_idx; i++) {
		struct buffer_head *buffer = bufvec->on_page[i].buffer;
		block_t physical = bufvec->on_page[i].block;
		get_bh(buffer);
		tux3_clear_buffer_dirty_for_io(buffer, sb, physical);
		bufvec_bio_add_buffer(bufvec, buffer);
	}
	prepare_and_unlock_page(page);

	bufvec->on_page_idx = 0;
}

/* Check whether "physical" is contiguous with bio */
static int bufvec_bio_is_contiguous(struct bufvec *bufvec, block_t physical)
{
	struct sb *sb = tux_sb(bufvec_inode(bufvec)->i_sb);
	struct bio *bio = bufvec->bio;
	block_t next;

	next = (block_t)bio->bi_iter.bi_sector + (bio->bi_iter.bi_size >> 9);
	return next == (physical << (sb->blockbits - 9));
}

/* Get the page of next candidate buffer. */
static struct page *bufvec_next_buffer_page(struct bufvec *bufvec)
{
	if (!list_empty(&bufvec->contig))
		return bufvec_contig_buf(bufvec)->b_page;

	if (bufvec->buffers && !list_empty(bufvec->buffers))
		return buffers_entry(bufvec->buffers->next)->b_page;

	return NULL;
}

/*
 * Prepare and submit I/O for specified range.
 *
 * This submits the contiguous range at once as much as possible.
 *
 * But if the page across on multiple ranges, we can't know when all
 * I/O was done on the page (and when we can clear the writeback flag).
 * So, we use different strategy. Those ranges are submitted as
 * multiple BIOs, and use BH_Update_Lock for exclusive check if I/O was
 * done.
 *
 * This doesn't guarantee all candidate buffers are submitted. E.g. if
 * the page across on multiple ranges, the page will be pending until
 * all physical addresses was specified.
 *
 * return value:
 * < 0 - error
 *   0 - success
 */
int bufvec_io(struct bufvec *bufvec, block_t physical, unsigned count)
{
	unsigned int i;
	int need_check = 0;

	trace("index %Lu, contig_count %u, physical %Lu, count %u",
	      bufvec_contig_index(bufvec), bufvec_contig_count(bufvec),
	      physical, count);

	/* FIXME: now only support WRITE */
	assert(op_is_write(bufvec->req_opf));
	assert(bufvec_contig_count(bufvec) >= count);

	if (bufvec->on_page_idx) {
		/*
		 * If there is the pending buffers on the page, and buffers
		 * was not contiguous, this is the complex case.
		 */
		need_check = 1;
	} else if (bufvec->bio && !bufvec_bio_is_contiguous(bufvec, physical)) {
		/*
		 * If new range is not contiguous with the pending bio,
		 * submit the pending bio.
		 */
		bufvec_submit_bio(bufvec);
	}

	/* Add buffers to bio for each page */
	for (i = 0; i < count; i++) {
		struct buffer_head *buffer = bufvec_contig_buf(bufvec);

		/* FIXME: need lock? (buffer is already owned by backend...) */
		bufvec->contig_count--;
		list_del_init(&buffer->b_assoc_buffers);

		/* Collect buffers on the same page */
		bufvec->on_page[bufvec->on_page_idx].buffer = buffer;
		bufvec->on_page[bufvec->on_page_idx].block = physical + i;
		bufvec->on_page_idx++;

		/* If next buffer isn't on same page, add buffers to bio */
		if (buffer->b_page != bufvec_next_buffer_page(bufvec)) {
			int multiple = 0;
			if (need_check) {
				need_check = 0;
				multiple = bufvec_is_multiple_ranges(bufvec);
			}

			if (multiple)
				bufvec_bio_add_multiple(bufvec);
			else
				bufvec_bio_add_page(bufvec);
		}
	}

	/* If no more buffer, submit the pending bio */
	if (bufvec->bio && !bufvec_next_buffer_page(bufvec))
		bufvec_submit_bio(bufvec);

	return 0;
}

static void bufvec_cancel_and_unlock_page(struct page *page,
					  const pgoff_t outside_index)
{
	/*
	 * If page is fully outside i_size, cancel dirty.
	 *
	 * If page is partially outside i_size, we have to check
	 * buffers. If all buffers aren't dirty, cancel dirty.
	 */
	if (page->index < outside_index)
		tux3_try_cancel_dirty_page(page);
	else
		cancel_dirty_page(page);

	unlock_page(page);
}

/* Cancel dirty buffers fully outside i_size */
static void bufvec_cancel_dirty_outside(struct bufvec *bufvec)
{
	struct sb *sb = tux_sb(bufvec_inode(bufvec)->i_sb);
	struct tux3_iattr_data *idata = bufvec->idata;
	struct page *page, *prev_page;
	struct buffer_head *buffer;
	pgoff_t outside_index;

	outside_index = (idata->i_size + (PAGE_SIZE - 1)) >> PAGE_SHIFT;

	buffer = buffers_entry(bufvec->buffers->next);
	page = prev_page = buffer->b_page;
	lock_page(page);
	while (1) {
		trace("cancel dirty: buffer %p, block %Lu",
		      buffer, bufindex(buffer));

		/* Cancel buffer dirty of outside i_size */
		list_del_init(&buffer->b_assoc_buffers);
		tux3_clear_buffer_dirty_for_io(buffer, sb, 0);
		tux3_clear_buffer_dirty_for_io_hack(buffer);

		if (list_empty(bufvec->buffers))
			break;

		buffer = buffers_entry(bufvec->buffers->next);
		if (buffer->b_page != prev_page) {
			bufvec_cancel_and_unlock_page(page, outside_index);

			prev_page = page;
			page = buffer->b_page;
			lock_page(page);
		}
	}
	bufvec_cancel_and_unlock_page(page, outside_index);
}

static int buffer_index_cmp(void *priv, struct list_head *a,
			    struct list_head *b)
{
	struct buffer_head *buf_a, *buf_b;

	buf_a = list_entry(a, struct buffer_head, b_assoc_buffers);
	buf_b = list_entry(b, struct buffer_head, b_assoc_buffers);

	/*
	 * Optimized version of the following:
	 *
	 * if (bufindex(buf_a) < bufindex(buf_b))
	 *	return -1;
	 * else if (bufindex(buf_a) > bufindex(buf_b))
	 *	return 1;
	 */
	if (buf_a->b_page->index < buf_b->b_page->index)
		return -1;
	else if (buf_a->b_page->index > buf_b->b_page->index)
		return 1;
	else {
		/* page_offset() is same, compare offset within page */
		if (buf_a->b_data < buf_b->b_data)
			return -1;
		if (buf_a->b_data > buf_b->b_data)
			return 1;
	}

	return 0;
}

static inline int tux3_call_io(struct inode *inode, struct bufvec *bufvec)
{
	return tux_inode(inode)->io(bufvec);
}

#include "buffer_writeback_common.c"

/*
 * I/O helper for physical index buffers (e.g. buffers on volmap)
 */
int __tux3_volmap_io(struct bufvec *bufvec, block_t physical, unsigned count)
{
	return blockio_vec(bufvec, physical, count);
}

/*
 * Volmap 2 phases I/O.
 *
 * We want to submit I/O by random order (not logical/physical order)
 * for volmap. Because, for example, we want to flush dleaf with
 * nearby data pages earlier than ileaf.
 *
 * But random order has the issue of dirty => clean state on the page.
 * Because we can't know when random order buffer I/O is done finally.
 * (while checking last I/O, another I/O can be started.)  This means
 * we would need some sort of IRQ safe locking for each submit_io and
 * end_io.
 *
 * To avoid IRQ safe locking for each buffers, this uses 2 phases I/O.
 *
 * The 1st phase just submits I/O and increment number of I/O in the delta,
 * and the 1st phase doesn't touch state of buffer or page at all.
 * With this, we just update on-disk data without modify in-core state.
 *
 * After the 1st phase was done, and confirmed all I/O in the delta
 * was done, we start the 2nd phase to update in-core state.  We know
 * all state can be clean in the delta.
 *
 * Because we have stable in-core state in the delta. If frontend
 * tried to re-dirty same buffer, frontend has to page forking then.
 * So at the 2nd phase, we walk all buffers, and we set all
 * buffers/pages pages clean.
 */

static void vol_early_end_io(struct bio *bio)
{
	struct buffer_head *buffer = bio->bi_private;
	struct address_space *mapping;

	/* FIXME: inode is still guaranteed to be available? */
	mapping = bufvec_bio_mapping(bio);

	/* Keep buffer dirty. State will be updated at 2st phase. */
	bufvec_buffer_end_io(bio, buffer);
	tux3_io_inflight_dec(tux_sb(mapping->host->i_sb)->ioinfo);
	bio_put(bio);
}

/* 1st phase I/O for volmap by random order */
int vol_early_io(enum req_opf req_opf, unsigned int req_flags,
		 struct sb *sb, struct buffer_head *buffer)
{
	int err;

	assert(buffer_dirty(buffer));
	/* FIXME: For now, this is only for write */
	assert(op_is_write(req_opf));

	list_move_tail(&buffer->b_assoc_buffers, &sb->phase2_buffers);

	tux3_io_inflight_inc(sb->ioinfo);
	err = blockio(req_opf, req_flags | tux3_io_req_flags(sb->ioinfo),
		      sb, buffer, bufindex(buffer), vol_early_end_io, buffer);
	if (err)
		tux3_io_inflight_dec(sb->ioinfo);

	return err;
}

static void bufvec_end_io_early(struct bio *bio)
{
	struct address_space *mapping;

	trace("bio %p, err %d", bio, bio->bi_status);

	/* FIXME: inode is still guaranteed to be available? */
	mapping = bufvec_bio_mapping(bio);

	/* Remove buffer from bio, then unlock buffer */
	while (1) {
		struct buffer_head *buffer = bufvec_bio_del_buffer(bio);
		if (!buffer)
			break;

		bufvec_buffer_end_io(bio, buffer);
	}

	tux3_io_inflight_dec(tux_sb(mapping->host->i_sb)->ioinfo);
	bio_put(bio);
}

static void bufvec_bio_add_early(struct bufvec *bufvec,
				 struct buffer_head *buffer, block_t physical)
{
	/* FIXME: inode is still guaranteed to be available? */
	struct sb *sb = tux_sb(bufvec_inode(bufvec)->i_sb);
	unsigned int length = bufsize(buffer);
	unsigned int offset = bh_offset(buffer);

	if (bufvec->bio) {
		if (bio_add_page(bufvec->bio, buffer->b_page, length, offset))
			return;
		/* Couldn't add buffer, submit current bio */
		bufvec_submit_bio(bufvec);
	}

	bufvec->bio = bufvec_bio_alloc(sb, bufvec_contig_count(bufvec) + 1,
				       physical, bufvec_end_io_early);

	if (!bio_add_page(bufvec->bio, buffer->b_page, length, offset))
		assert(0);	/* why? */
	bufvec_bio_add_buffer(bufvec, buffer);
}

/* 1st phase I/O for volmap by sequential order */
int tux3_volmap_early_io(struct bufvec *bufvec)
{
	struct sb *sb = tux_sb(bufvec_inode(bufvec)->i_sb);
	block_t physical = bufvec_contig_index(bufvec);
	unsigned i, count = bufvec_contig_count(bufvec);
	int err = 0;

	/* FIXME: For now, this is only for write */
	assert(op_is_write(bufvec->req_opf));

	/* Add buffers to bio for each page */
	for (i = 0; i < count; i++) {
		struct buffer_head *buffer = bufvec_contig_buf(bufvec);

		/* FIXME: need lock? (buffer is already owned by backend...) */
		bufvec->contig_count--;
		/* FIXME: this can be replaced by list_splice() */
		list_move_tail(&buffer->b_assoc_buffers, &sb->phase2_buffers);

		bufvec_bio_add_early(bufvec, buffer, physical + i);
	}
	/* Submit the pending bio */
	if (bufvec->bio)
		bufvec_submit_bio(bufvec);

	return err;
}

static void clean_state(struct sb *sb, struct buffer_head *on_page[],
			int on_page_idx)
{
	struct inode *volmap = sb->volmap;
	struct page *page = on_page[0]->b_page;
	int i;

	/*
	 * This is using volmap->i_size directly, however we may be
	 * better to use idata->i_size. (e.g. idata->i_size may help
	 * device resize?)
	 */
	prepare_and_lock_page(page, on_page_idx, volmap->i_size, 1);

	for (i = 0; i < on_page_idx; i++) {
		struct buffer_head *buffer = on_page[i];
		tux3_clear_buffer_dirty_for_io(buffer, sb, bufindex(buffer));
		tux3_clear_buffer_dirty_for_io_hack(buffer);
	}
	prepare_and_unlock_page(page);

	end_page_writeback(page);
}

static struct buffer_head *next_buf(struct list_head *head)
{
	return list_entry(head->next, struct buffer_head, b_assoc_buffers);
}

/* 2nd phase I/O for volmap (I.e. clean buffer/page state) */
int tux3_volmap_clean_io(struct inode *inode)
{
	struct sb *sb = tux_sb(inode->i_sb);
	struct list_head *head = &sb->phase2_buffers;
	struct buffer_head *on_page[BUFS_PER_PAGE];
	int done, on_page_idx;

	if (list_empty(head))
		return 0;

	/* FIXME: we can't skip sort? */
	/* Sort by bufindex() */
	list_sort(NULL, head, buffer_index_cmp);

	done = on_page_idx = 0;
	while (!done) {
		struct buffer_head *buffer = next_buf(head);

		list_del_init(&buffer->b_assoc_buffers);
		on_page[on_page_idx] = buffer;
		on_page_idx++;

		done = list_empty(head);
		if (done || on_page[0]->b_page != next_buf(head)->b_page) {
			clean_state(sb, on_page, on_page_idx);
			on_page_idx = 0;
		}
	}

	return 0;
}
