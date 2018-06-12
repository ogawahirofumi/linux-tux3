/*
 * Copied some writeback library functions to teach about PageForked().
 *
 * We should check the update of original functions, and sync with it.
 */

#include <linux/rmap.h>		/* for page_mkclean() */

/*
 * Clear a page's dirty flag, while caring for dirty memory accounting.
 * Returns true if the page was previously dirty.
 *
 * This is for preparing to put the page under writeout.  We leave the page
 * tagged as dirty in the radix tree so that a concurrent write-for-sync
 * can discover it via a PAGECACHE_TAG_DIRTY walk.  The ->writepage
 * implementation will run either set_page_writeback() or set_page_dirty(),
 * at which stage we bring the page's dirty flag and radix-tree dirty tag
 * back into sync.
 *
 * This incoherency between the page's dirty flag and radix-tree tag is
 * unfortunate, but it only exists while the page is locked.
 */
static int tux3_clear_page_dirty_for_io(struct page *page, int outside)
{
	struct address_space *mapping = page->mapping;
	int ret = 0;

	BUG_ON(!PageLocked(page));

	if (mapping && mapping_cap_account_dirty(mapping)) {
		struct inode *inode = mapping->host;
		struct bdi_writeback *wb;
		struct wb_lock_cookie cookie = {};

		/*
		 * Yes, Virginia, this is indeed insane.
		 *
		 * We use this sequence to make sure that
		 *  (a) we account for dirty stats properly
		 *  (b) we tell the low-level filesystem to
		 *      mark the whole page dirty if it was
		 *      dirty in a pagetable. Only to then
		 *  (c) clean the page again and return 1 to
		 *      cause the writeback.
		 *
		 * This way we avoid all nasty races with the
		 * dirty bit in multiple places and clearing
		 * them concurrently from different threads.
		 *
		 * Note! Normally the "set_page_dirty(page)"
		 * has no effect on the actual dirty bit - since
		 * that will already usually be set. But we
		 * need the side effects, and it can help us
		 * avoid races.
		 *
		 * We basically use the page "master dirty bit"
		 * as a serialization point for all the different
		 * threads doing their things.
		 */
		/* If PageForked(), don't touch PTE and don't dirty */
		if (!PageForked(page) && page_mkclean(page)) {
			/* FIXME: we should not need to call this */
			if (!outside)
				set_page_dirty(page);
		}
		/*
		 * We carefully synchronise fault handlers against
		 * installing a dirty pte and marking the page dirty
		 * at this point.  We do this by having them hold the
		 * page lock while dirtying the page, and pages are
		 * always locked coming in here, so we get the desired
		 * exclusion.
		 */
		wb = unlocked_inode_to_wb_begin(inode, &cookie);
		if (TestClearPageDirty(page)) {
			dec_lruvec_page_state(page, NR_FILE_DIRTY);
			dec_zone_page_state(page, NR_ZONE_WRITE_PENDING);
			dec_wb_stat(wb, WB_RECLAIMABLE);
			ret = 1;
		}
		unlocked_inode_to_wb_end(inode, &cookie);
		return ret;
	}
	return TestClearPageDirty(page);
}

static void
__tux3_test_set_page_writeback(struct page *page, bool keep_write,
			       bool old_writeback)
{
	struct address_space *mapping = page->mapping;

	lock_page_memcg(page);
	if (mapping && mapping_use_writeback_tags(mapping)) {
		struct inode *inode = mapping->host;
		struct backing_dev_info *bdi = inode_to_bdi(inode);
		unsigned long flags;

		spin_lock_irqsave(&mapping->tree_lock, flags);
		if (!old_writeback) {
			bool on_wblist;

			/* If PageForked(), don't touch tag */
			if (PageForked(page))
				goto skip_tag_set;

			on_wblist = mapping_tagged(mapping,
						   PAGECACHE_TAG_WRITEBACK);

			radix_tree_tag_set(&mapping->page_tree,
					   page_index(page),
					   PAGECACHE_TAG_WRITEBACK);

#if 0 /* sb_mark_inode_writeback() is not exported to module. And tux3
       * sync all with ->sync_fs(), thus this just makes slower us. */
			/*
			 * We can come through here when swapping anonymous
			 * pages, so we don't necessarily have an inode to track
			 * for sync.
			 */
			if (mapping->host && !on_wblist)
				sb_mark_inode_writeback(mapping->host);
#endif

skip_tag_set:
			if (bdi_cap_account_writeback(bdi))
				inc_wb_stat(inode_to_wb(inode), WB_WRITEBACK);
		}
		/* If PageForked(), don't touch tag */
		if (!PageDirty(page) && !PageForked(page))
			radix_tree_tag_clear(&mapping->page_tree,
						page_index(page),
						PAGECACHE_TAG_DIRTY);
		if (!keep_write)
			radix_tree_tag_clear(&mapping->page_tree,
					     page_index(page),
					     PAGECACHE_TAG_TOWRITE);
		spin_unlock_irqrestore(&mapping->tree_lock, flags);
	} else {
		BUG();
	}
	if (!old_writeback) {
		inc_lruvec_page_state(page, NR_WRITEBACK);
		inc_zone_page_state(page, NR_ZONE_WRITE_PENDING);
	}
	unlock_page_memcg(page);
}

#define tux3_test_set_page_writeback(page, old_writeback)		\
	__tux3_test_set_page_writeback(page, false, old_writeback)
#define tux3_test_set_page_writeback_keepwrite(page, old_writeback)	\
	__tux3_test_set_page_writeback(page, true, old_writeback)
