#ifndef TUX3_IOINFO_H
#define TUX3_IOINFO_H

/*
 * Helper for waiting I/O, and commit related info
 */

enum {
	FLUSH_NORMAL	= 0,		/* Normal flush */
	FLUSH_SYNC	= 1 << 0,	/* Synchronous flush (e.g. fsync(2)) */
	__NO_UNIFY	= 1 << 1,	/* No unify */
	__FORCE_DELTA	= 1 << 2,	/* Force delta even if no dirty */
	__FORCE_UNIFY	= 1 << 3,	/* Force unify */

	/* Debug: Force flush even without unify if no dirty */
	FORCE_DELTA	= __FORCE_DELTA | __NO_UNIFY,
	/* Debug: Force flush with unify even if no dirty */
	FORCE_UNIFY	= __FORCE_DELTA | __FORCE_UNIFY,
};

struct ioinfo {
	refcount_t inflight;		/* In-flight I/O count */
	struct completion done;		/* completion for in-flight I/O */

	int flush_flags;		/* Flush type */
	unsigned int req_flags;		/* Additional REQ_ flags */
};

static inline void tux3_io_inflight_inc(struct ioinfo *ioinfo)
{
	refcount_inc(&ioinfo->inflight);
}

static inline void tux3_io_inflight_dec(struct ioinfo *ioinfo)
{
	if (refcount_dec_and_test(&ioinfo->inflight))
		complete(&ioinfo->done);
}

static inline void tux3_io_init(struct ioinfo *ioinfo, int flush_flags)
{
	ioinfo->flush_flags = flush_flags;
	ioinfo->req_flags = (flush_flags & FLUSH_SYNC) ? REQ_SYNC : 0;

	/*
	 * Grab 1 to prevent the partial complete until all I/O is
	 * submitted
	 */
	init_completion(&ioinfo->done);
	refcount_set(&ioinfo->inflight, 1);
}

static inline void tux3_io_wait(struct ioinfo *ioinfo)
{
	/* All I/O was submitted, release initial 1, then wait I/O */
	tux3_io_inflight_dec(ioinfo);
	wait_for_completion_io(&ioinfo->done);
}

/* This req_flags is added to all I/O request counted in ->ioinfo */
static inline unsigned int tux3_io_req_flags(struct ioinfo *ioinfo)
{
	return ioinfo->req_flags;
}

/* Flags for flush type */
static inline int tux3_io_flush_flags(struct ioinfo *ioinfo)
{
	return ioinfo->flush_flags;
}

#endif /* !TUX3_IOINFO_H */
