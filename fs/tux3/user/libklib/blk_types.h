#ifndef LIBKLIB_BLK_TYPES_H
#define LIBKLIB_BLK_TYPES_H

#define REQ_OP_BITS	8
#define REQ_OP_MASK	((1 << REQ_OP_BITS) - 1)
#define REQ_FLAG_BITS	24

enum req_opf {
	/* read sectors from the device */
	REQ_OP_READ		= 0,
	/* write sectors to the device */
	REQ_OP_WRITE		= 1,
	/* flush the volatile write cache */
	REQ_OP_FLUSH		= 2,
	/* discard sectors */
	REQ_OP_DISCARD		= 3,
	/* get zone information */
	REQ_OP_ZONE_REPORT	= 4,
	/* securely erase sectors */
	REQ_OP_SECURE_ERASE	= 5,
	/* seset a zone write pointer */
	REQ_OP_ZONE_RESET	= 6,
	/* write the same sector many times */
	REQ_OP_WRITE_SAME	= 7,
	/* write the zero filled sector many times */
	REQ_OP_WRITE_ZEROES	= 8,

	REQ_OP_LAST,
};

enum req_flag_bits {
	__REQ_FAILFAST_DEV =	/* no driver retries of device errors */
		REQ_OP_BITS,
	__REQ_FAILFAST_TRANSPORT, /* no driver retries of transport errors */
	__REQ_FAILFAST_DRIVER,	/* no driver retries of driver errors */
	__REQ_SYNC,		/* request is sync (sync write or read) */
	__REQ_META,		/* metadata io request */
	__REQ_PRIO,		/* boost priority in cfq */
	__REQ_NOMERGE,		/* don't touch this for merging */
	__REQ_IDLE,		/* anticipate more IO after this one */
	__REQ_INTEGRITY,	/* I/O includes block integrity payload */
	__REQ_FUA,		/* forced unit access */
	__REQ_PREFLUSH,		/* request for cache flush */
	__REQ_RAHEAD,		/* read ahead, can fail anytime */
	__REQ_BACKGROUND,	/* background IO */
	__REQ_NR_BITS,		/* stops here */
};

#define REQ_FAILFAST_DEV	(1ULL << __REQ_FAILFAST_DEV)
#define REQ_FAILFAST_TRANSPORT	(1ULL << __REQ_FAILFAST_TRANSPORT)
#define REQ_FAILFAST_DRIVER	(1ULL << __REQ_FAILFAST_DRIVER)
#define REQ_SYNC		(1ULL << __REQ_SYNC)
#define REQ_META		(1ULL << __REQ_META)
#define REQ_PRIO		(1ULL << __REQ_PRIO)
#define REQ_NOMERGE		(1ULL << __REQ_NOMERGE)
#define REQ_IDLE		(1ULL << __REQ_IDLE)
#define REQ_INTEGRITY		(1ULL << __REQ_INTEGRITY)
#define REQ_FUA			(1ULL << __REQ_FUA)
#define REQ_PREFLUSH		(1ULL << __REQ_PREFLUSH)
#define REQ_RAHEAD		(1ULL << __REQ_RAHEAD)
#define REQ_BACKGROUND		(1ULL << __REQ_BACKGROUND)

static inline bool op_is_write(unsigned int op)
{
	return (op & 1);
}

#endif /* !LIBKLIB_BLK_TYPES_H */
