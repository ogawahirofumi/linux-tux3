#ifndef LIBKLIB_BLK_TYPES_H
#define LIBKLIB_BLK_TYPES_H

#define REQ_SYNC	(1ULL << 3)
#define REQ_META	(1ULL << 4)
#define REQ_PRIO	(1ULL << 5)
#define REQ_NOIDLE	(1ULL << 6)

#define REQ_RAHEAD	(1ULL << 10)

#define REQ_FUA		(1ULL << 8)
#define REQ_PREFLUSH	(1ULL << 9)

#define RW_MASK		REQ_OP_WRITE

#define READ		REQ_OP_READ
#define WRITE		REQ_OP_WRITE

enum req_op {
	REQ_OP_READ,
	REQ_OP_WRITE,
	REQ_OP_DISCARD,		/* request to discard sectors */
	REQ_OP_SECURE_ERASE,	/* request to securely erase sectors */
	REQ_OP_WRITE_SAME,	/* write same block many times */
	REQ_OP_FLUSH,		/* request for cache flush */
};

#define REQ_OP_BITS 3

#endif /* !LIBKLIB_BLK_TYPES_H */
