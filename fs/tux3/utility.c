/*
 * Utility functions.
 *
 * Copyright (c) 2009-2014 Daniel Phillips
 * Copyright (c) 2009-2014 OGAWA Hirofumi
 */

#ifdef __KERNEL__
#include "tux3.h"

static int vecio(enum req_opf req_opf, unsigned int req_flags,
		 struct block_device *dev, loff_t offset,
		 unsigned vecs, struct bio_vec *vec,
		 bio_end_io_t endio, void *bio_private)
{
	struct bio *bio;

	BUG_ON(vecs > BIO_MAX_VECS);

	bio = bio_alloc(GFP_NOIO, vecs);
	if (!bio)
		return -ENOMEM;

	bio_set_dev(bio, dev);
	bio->bi_iter.bi_sector = offset >> 9;
	bio->bi_end_io = endio;
	bio->bi_private = bio_private;
	bio->bi_vcnt = vecs;
	memcpy(bio->bi_io_vec, vec, sizeof(*vec) * vecs);
	while (vecs--)
		bio->bi_iter.bi_size += bio->bi_io_vec[vecs].bv_len;

	bio->bi_write_hint = WRITE_LIFE_NOT_SET;
	bio_set_op_attrs(bio, req_opf, req_flags);
	submit_bio(bio);

	return 0;
}

struct biosync {
	struct completion done;
	int err;
};

static void syncio_end_io(struct bio *bio)
{
	struct biosync *sync = bio->bi_private;
	sync->err = blk_status_to_errno(bio->bi_status);
	complete(&sync->done);
	bio_put(bio);
}

static int syncio(enum req_opf req_opf, unsigned int req_flags,
		  struct block_device *dev, loff_t offset,
		  unsigned vecs, struct bio_vec *vec)
{
	struct biosync sync = {
		.done = COMPLETION_INITIALIZER_ONSTACK(sync.done)
	};
	sync.err = vecio(req_opf, req_flags, dev, offset, vecs, vec,
			 syncio_end_io, &sync);
	if (!sync.err)
		wait_for_completion_io(&sync.done);
	return sync.err;
}

int devio_sync(enum req_opf req_opf, unsigned int req_flags,
	       struct block_device *dev, loff_t offset, void *data,
	       unsigned len)
{
	struct bio_vec vec = {
		.bv_page	= virt_to_page(data),
		.bv_offset	= offset_in_page(data),
		.bv_len		= len,
	};

	return syncio(req_opf, req_flags, dev, offset, 1, &vec);
}

int blockio(enum req_opf req_opf, unsigned int req_flags,
	    struct sb *sb, struct buffer_head *buffer, block_t block,
	    bio_end_io_t endio, void *info)
{
	struct bio_vec vec = {
		.bv_page	= buffer->b_page,
		.bv_offset	= bh_offset(buffer),
		.bv_len		= sb->blocksize,
	};

	return vecio(req_opf, req_flags, sb_dev(sb), block << sb->blockbits, 1,
		     &vec, endio, info);
}

int blockio_sync(enum req_opf req_opf, unsigned int req_flags, struct sb *sb,
		 struct buffer_head *buffer, block_t block)
{
	struct bio_vec vec = {
		.bv_page	= buffer->b_page,
		.bv_offset	= bh_offset(buffer),
		.bv_len		= sb->blocksize,
	};

	return syncio(req_opf, req_flags, sb_dev(sb),
		      block << sb->blockbits, 1, &vec);
}

/*
 * bufvec based I/O.  This takes the bufvec has contiguous range, and
 * will submit the count of buffers to block (physical address).
 *
 * If there was I/O error, it would be handled in ->bi_end_bio()
 * completion.
 */
int blockio_vec(struct bufvec *bufvec, block_t block, unsigned count)
{
	return bufvec_io(bufvec, block, count);
}

void hexdump(void *data, unsigned size)
{
	print_hex_dump(KERN_INFO, "", DUMP_PREFIX_ADDRESS, 16, 1, data, size, 1);
}

/*
 * Message helpers
 */

void __tux3_msg(struct sb *sb, const char *level, const char *prefix,
		const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	printk("%sTUX3-fs%s (%s): %pV\n", level, prefix,
	       vfs_sb(sb)->s_id, &vaf);
	va_end(args);
}

void __tux3_fs_error(struct sb *sb, const char *func, unsigned int line,
		     const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	printk(KERN_ERR "TUX3-fs error (%s): %s:%d: %pV\n",
	       vfs_sb(sb)->s_id, func, line, &vaf);
	va_end(args);

	BUG();		/* FIXME: maybe panic() or SB_RDONLY */
}

void __tux3_dbg(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vprintk(fmt, args);
	va_end(args);
}
#endif /* __KERNEL__ */

/* Bitmap operations... try to use linux/lib/bitmap.c */

void set_bits(u8 *bitmap, unsigned start, unsigned count)
{
	unsigned limit = start + count;
	unsigned lmask = (~0U << (start & 7)) & 0xff; // little endian!!!
	unsigned rmask = ((1U << (limit & 7)) - 1) & 0xff; // little endian!!!
	unsigned loff = start >> 3, roff = limit >> 3;

	if (loff == roff) {
		bitmap[loff] |= lmask & rmask;
		return;
	}
	bitmap[loff] |= lmask;
	memset(bitmap + loff + 1, -1, roff - loff - 1);
	if (rmask)
		bitmap[roff] |= rmask;
}

void clear_bits(u8 *bitmap, unsigned start, unsigned count)
{
	unsigned limit = start + count;
	unsigned lmask = (~0U << (start & 7)) & 0xff; // little endian!!!
	unsigned rmask = ((1U << (limit & 7)) - 1) & 0xff; // little endian!!!
	unsigned loff = start >> 3, roff = limit >> 3;

	if (loff == roff) {
		bitmap[loff] &= ~lmask | ~rmask;
		return;
	}
	bitmap[loff] &= ~lmask;
	memset(bitmap + loff + 1, 0, roff - loff - 1);
	if (rmask)
		bitmap[roff] &= ~rmask;
}

int all_set(u8 *bitmap, unsigned start, unsigned count)
{
#if 1
	/* Bitmap must be array of "unsigned long" */
	unsigned limit = start + count;
	/* Find zero bit in range. If not found, all are non-zero.  */
	return find_next_zero_bit_le(bitmap, limit, start) == limit;
#else
	unsigned limit = start + count;
	unsigned lmask = (~0U << (start & 7)) & 0xff;	// little endian!!!
	unsigned rmask = ((1U << (limit & 7)) - 1) & 0xff; // little endian!!!
	unsigned loff = start >> 3, roff = limit >> 3;

	if (loff == roff) {
		unsigned mask = lmask & rmask;
		return (bitmap[loff] & mask) == mask;
	}
	for (unsigned i = loff + 1; i < roff; i++)
		if (bitmap[i] != 0xff)
			return 0;
	return	(bitmap[loff] & lmask) == lmask &&
		(!rmask || (bitmap[roff] & rmask) == rmask);
#endif
}

int all_clear(u8 *bitmap, unsigned start, unsigned count)
{
#if 1
	/* Bitmap must be array of "unsigned long" */
	unsigned limit = start + count;
	/* Find non-zero bit in range. If not found, all are zero.  */
	return find_next_bit_le(bitmap, limit, start) == limit;
#else
	unsigned limit = start + count;
	unsigned lmask = (~0U << (start & 7)) & 0xff; // little endian!!!
	unsigned rmask = ((1U << (limit & 7)) - 1) & 0xff; // little endian!!!
	unsigned loff = start >> 3, roff = limit >> 3;

	if (loff == roff) {
		unsigned mask = lmask & rmask;
		return !(bitmap[loff] & mask);
	}
	for (unsigned i = loff + 1; i < roff; i++)
		if (bitmap[i])
			return 0;
	return	!(bitmap[loff] & lmask) &&
		(!rmask || !(bitmap[roff] & rmask));
#endif
}

int bytebits(u8 c)
{
	unsigned count = 0;

	for (; c; c >>= 1)
		count += c & 1;
	return count;
}
