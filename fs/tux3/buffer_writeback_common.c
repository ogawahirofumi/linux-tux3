/*
 * Common write back block buffers.
 *
 * Copyright (c) 2008-2017 OGAWA Hirofumi
 */

#include "ioinfo.h"

/*
 * Try to add buffer to bufvec as contiguous range.
 *
 * return value:
 * 1 - success
 * 0 - fail to add
 */
int bufvec_contig_add(struct bufvec *bufvec, struct buffer_head *buffer)
{
	unsigned contig_count = bufvec_contig_count(bufvec);

	if (contig_count) {
		block_t last;

		/* Check contig_count limit */
		if (bufvec_contig_count(bufvec) == MAX_BUFVEC_COUNT)
			return 0;

		/* Check if buffer is logically contiguous */
		last = bufvec_contig_last_index(bufvec);
		if (last != bufindex(buffer) - 1)
			return 0;
	}

	bufvec_buffer_move_to_contig(bufvec, buffer);

	return 1;
}

/*
 * Try to collect logically contiguous dirty range from bufvec->buffers.
 *
 * return value:
 * 1 - there is buffers for I/O
 * 0 - no buffers for I/O
 */
static int bufvec_contig_collect(struct bufvec *bufvec)
{
	struct sb *sb = tux_sb(bufvec_inode(bufvec)->i_sb);
	struct tux3_iattr_data *idata = bufvec->idata;
	struct buffer_head *buffer;
	block_t last_index, next_index, outside_block;

	/* If there is in-progress contiguous range, leave as is */
	if (bufvec_contig_count(bufvec))
		return 1;
	assert(!list_empty(bufvec->buffers));

	outside_block = (idata->i_size + sb->blockmask) >> sb->blockbits;

	buffer = buffers_entry(bufvec->buffers->next);
	next_index = bufindex(buffer);
	/* If next buffer is fully outside i_size, clear dirty */
	if (next_index >= outside_block) {
		bufvec_cancel_dirty_outside(bufvec);
		return 0;
	}

	do {
		/* Check contig_count limit */
		if (bufvec_contig_count(bufvec) == MAX_BUFVEC_COUNT)
			break;
		bufvec_buffer_move_to_contig(bufvec, buffer);

		if (list_empty(bufvec->buffers))
			break;

		buffer = buffers_entry(bufvec->buffers->next);
		last_index = next_index;
		next_index = bufindex(buffer);

		/* If next buffer is fully outside i_size, clear dirty */
		if (next_index >= outside_block) {
			bufvec_cancel_dirty_outside(bufvec);
			break;
		}
	} while (last_index == next_index - 1);

	return !!bufvec_contig_count(bufvec);
}

/*
 * Flush buffers in head
 */
int flush_list(struct inode *inode, struct tux3_iattr_data *idata,
	       struct list_head *head, unsigned int req_flags)
{
	struct sb *sb = tux_sb(inode->i_sb);
	struct bufvec bufvec;
	int err = 0;

	/* FIXME: on error path, we have to do something for buffer state */

	if (list_empty(head))
		return 0;

	bufvec_init(&bufvec, REQ_OP_WRITE,
		    req_flags | tux3_io_req_flags(sb->ioinfo),
		    mapping(inode), head, idata);

	/* Sort by bufindex() */
	list_sort(NULL, head, buffer_index_cmp);

	while (bufvec_next_buffer_page(&bufvec)) {
		/* Collect contiguous buffer range */
		if (bufvec_contig_collect(&bufvec)) {
			policy_extents(&bufvec);

			err = tux3_call_io(inode, &bufvec);
			if (err)
				break;
		}
	}

	bufvec_free(&bufvec);
	remember_dleaf(sb, NULL);

	return err;
}
