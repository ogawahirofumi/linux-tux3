/*
 * Write back buffers
 */

/* Block plugging (stub) */
void blk_start_plug(struct blk_plug *plug)
{
}

void blk_finish_plug(struct blk_plug *plug)
{
}

/*
 * Helper for buffer vector I/O.
 */

static inline struct buffer_head *buffers_entry(struct list_head *x)
{
	return list_entry(x, struct buffer_head, link);
}

#define MAX_BUFVEC_COUNT	UINT_MAX

/* Initialize bufvec */
void bufvec_init(struct bufvec *bufvec, enum req_opf req_opf,
		 unsigned int req_flags, map_t *map,
		 struct list_head *head, struct tux3_iattr_data *idata)
{
	INIT_LIST_HEAD(&bufvec->contig);
	INIT_LIST_HEAD(&bufvec->for_io);
	bufvec->req_opf		= req_opf;
	bufvec->req_flags	= req_flags;
	bufvec->buffers		= head;
	bufvec->contig_count	= 0;
	bufvec->idata		= idata;
	bufvec->map		= map;
	bufvec->end_io		= NULL;
}

void bufvec_free(struct bufvec *bufvec)
{
	/* FIXME: on error path, this will happens */
	assert(!bufvec->buffers || list_empty(bufvec->buffers));
	assert(list_empty(&bufvec->contig));
	assert(list_empty(&bufvec->for_io));
}

static inline void bufvec_buffer_move_to_contig(struct bufvec *bufvec,
						struct buffer_head *buffer)
{
	list_move_tail(&buffer->link, &bufvec->contig);
	bufvec->contig_count++;
}

static void buffer_io_done(struct buffer_head *buffer, int err,
			   bufvec_end_io_t end_io)
{
	list_del_init(&buffer->link);
	end_io(buffer, err);
}

static void bufvec_io_done(struct bufvec *bufvec, int err)
{
	struct list_head *head = &bufvec->for_io;

	while (!list_empty(head)) {
		struct buffer_head *buffer = buffers_entry(head->next);
		buffer_io_done(buffer, err, bufvec->end_io);
	}
}

/* Get the next candidate buffer. */
static struct buffer_head *bufvec_next_buffer_page(struct bufvec *bufvec)
{
	if (!list_empty(&bufvec->contig))
		return bufvec_contig_buf(bufvec);

	if (bufvec->buffers && !list_empty(bufvec->buffers))
		return buffers_entry(bufvec->buffers->next);

	return NULL;
}

/*
 * Prepare and submit I/O for specified range.
 *
 * This doesn't guarantee all candidate buffers are prepared for
 * I/O. It might be limited by device or block layer.
 *
 * return value:
 * < 0 - error
 *   0 - success
 */
int bufvec_io(struct bufvec *bufvec, block_t physical, unsigned count)
{
	struct sb *sb = tux_sb(bufvec_inode(bufvec)->i_sb);
	struct iovec *iov;
	unsigned i, iov_count;
	int err;

	assert(count <= bufvec_contig_count(bufvec));

	iov = malloc(sizeof(*iov) * count);
	if (iov == NULL)
		return -ENOMEM;
	iov_count = 0;

	/* Add buffers for I/O */
	for (i = 0; i < count; i++) {
		struct buffer_head *buffer = bufvec_contig_buf(bufvec);

		/* buffer will be re-added into per-state list after I/O done */
		list_move_tail(&buffer->link, &bufvec->for_io);
		bufvec->contig_count--;

		iov[i].iov_base = bufdata(buffer);
		iov[i].iov_len = bufsize(buffer);
		iov_count++;
	}
	assert(i > 0);

	err = devio_vec(bufvec->req_opf, bufvec->req_flags, sb_dev(sb),
			physical << sb->blockbits, iov, iov_count);
	bufvec_io_done(bufvec, err);

	free(iov);

	return 0;
}

/*
 * Call completion without I/O. I.e. change buffer state without I/O.
 */
void bufvec_complete_without_io(struct bufvec *bufvec, unsigned count)
{
	unsigned i;

	assert(count <= bufvec_contig_count(bufvec));

	/* Add buffers for completion */
	for (i = 0; i < count; i++) {
		struct buffer_head *buffer = bufvec_contig_buf(bufvec);

		/* buffer will be re-added into per-state list after I/O done */
		list_move_tail(&buffer->link, &bufvec->for_io);
		bufvec->contig_count--;
	}
	assert(i > 0);

	bufvec_io_done(bufvec, 0);
}

static void cancel_buffer_dirty(struct bufvec *bufvec,
				struct buffer_head *buffer)
{
	if (tux3_inode_test_flag(TUX3_I_NO_DELTA, bufvec_inode(bufvec)))
		__clear_buffer_dirty_for_endio(buffer, 0);
	else
		clear_buffer_dirty_for_endio(buffer, 0);
}

/* Cancel dirty buffers fully outside i_size */
static void bufvec_cancel_dirty_outside(struct bufvec *bufvec)
{
	struct buffer_head *buffer;

	while (!list_empty(bufvec->buffers)) {
		buffer = buffers_entry(bufvec->buffers->next);
		buftrace("cancel dirty: buffer %p, block %Lu",
			 buffer, bufindex(buffer));

		list_del_init(&buffer->link);
		/* Cancel buffer dirty of outside i_size */
		cancel_buffer_dirty(bufvec, buffer);
	}
}

static int buffer_index_cmp(void *priv, const struct list_head *a,
			    const struct list_head *b)
{
	struct buffer_head *buf_a = list_entry(a, struct buffer_head, link);
	struct buffer_head *buf_b = list_entry(b, struct buffer_head, link);

	if (bufindex(buf_a) < bufindex(buf_b))
		return -1;
	else if (bufindex(buf_a) > bufindex(buf_b))
		return 1;
	return 0;
}

static inline int tux3_call_io(struct inode *inode, struct bufvec *bufvec)
{
	return mapping(inode)->io(bufvec);
}

#include "../buffer_writeback_common.c"

/* FIXME: hack to disable vol_early_io for testing purpose. (we would
 * want to remove vol_early_io itself.) */
bool disable_vol_early_io;

/* 1st phase I/O for volmap by random order */
int vol_early_io(enum req_opf req_opf, unsigned int req_flags,
		 struct sb *sb, struct buffer_head *buffer)
{
	int err;
	if (disable_vol_early_io)
		return 0;
	assert(buffer_dirty(buffer));
	err = blockio_sync(req_opf, req_flags | tux3_io_req_flags(sb->ioinfo),
			   sb, buffer, bufindex(buffer));
	buffer_io_done(buffer, err, __clear_buffer_dirty_for_endio);
	return err;
}

/* 2nd phase I/O for volmap (I.e. clean buffer/page state) */
int tux3_volmap_clean_io(struct inode *inode)
{
	/* All is done at 1st phase. Nothing to do in userland */
	return 0;
}
