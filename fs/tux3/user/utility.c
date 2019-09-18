#include "tux3user.h"
#include "diskio.h"
#include "fault_inject.h"

#include "buffer.c"
#include "hexdump.c"

#ifndef trace
#define trace trace_off
#endif

#include "../utility.c"

static int preflush(unsigned int req_flags, struct dev *dev)
{
	if (req_flags & REQ_PREFLUSH) {
		if (fdatasync(dev->fd) < 0)
			return -errno;
	}
	return 0;
}

static int postflush(unsigned int req_flags, struct dev *dev)
{
	if (req_flags & REQ_FUA) {
		if (fdatasync(dev->fd) < 0)
			return -errno;
	}
	return 0;
}

int devio_vec(enum req_opf req_opf, unsigned int req_flags, struct dev *dev,
	      loff_t offset, struct iovec *iov, unsigned iovcnt)
{
	int err;

	if (op_is_write(req_opf))
		fault_return("io:write:", -EIO);
	else
		fault_return("io:read:", -EIO);

	err = preflush(req_flags, dev);
	if (err)
		return err;

	err = iovabs(dev->fd, iov, iovcnt, op_is_write(req_opf), offset);
	if (err)
		return err;

	return postflush(req_flags, dev);
}

int devio_sync(enum req_opf req_opf, unsigned int req_flags,
	       struct dev *dev, loff_t offset, void *data,
	       unsigned len)
{
	struct iovec iov = {
		.iov_base	= data,
		.iov_len	= len,
	};
	return devio_vec(req_opf, req_flags, dev, offset, &iov, 1);
}

int blockio_sync(enum req_opf req_opf, unsigned int req_flags, struct sb *sb,
		 struct buffer_head *buffer, block_t block)
{
	trace("%s: buffer %p, block %Lx",
	      op_is_write(req_opf) ? "write" : "read", buffer, block);
	return devio_sync(req_opf, req_flags, sb_dev(sb),
			  block << sb->blockbits,
			  bufdata(buffer), sb->blocksize);
}

int blockio_vec(struct bufvec *bufvec, block_t block, unsigned count)
{
	trace("%s: bufvec %p, count %u, block %Lx",
	      op_is_write(bufvec->req_opf) ? "write" : "read",
	      bufvec, count, block);
	return bufvec_io(bufvec, block, count);
}

/*
 * Message helpers
 */

void __tux3_msg(struct sb *sb, const char *level, const char *prefix,
		const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
}

void __tux3_fs_error(struct sb *sb, const char *func, unsigned int line,
		     const char *fmt, ...)
{
	va_list args;

	printf("Error: %s:%d: ", func, line);
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
	printf("\n");

	assert(0);		/* FIXME: what to do here? */
}

void __tux3_dbg(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
}
