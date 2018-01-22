/*
 * For the fixed size key/data, data structure like "dictionary" (or
 * associative list) in a block.
 *
 *     +----------------------+
 *     |        count         |
 *  0n +======================+            <- first_key
 *     |        key[0]        |--------+
 *  1n +----------------------+        |
 *     |        key[1]        |--+     |
 *     +----------------------+  |     |   <- limit_key
 *     |         ...          | pair  pair
 * -2x +----------------------+  |     |   <- limit_data
 *     |       data[1]        |--+     |
 * -1x +----------------------+        |
 *     |       data[0]        |--------+
 *     +----------------------+            <- top_data
 */

/* Endian of fields */
#if !defined(FBDICT_LITTLE_ENDIAN) && !defined(FBDICT_BIG_ENDIAN)
#error "FBDICT_{LITTLE,BIG}_ENDIAN are undefined"
#endif
/* Size of one key */
#ifndef FBDICT_KEY_SIZE
#error "FBDICT_KEY_SIZE is undefined"
#endif
/* Size of one data */
#ifndef FBDICT_DATA_SIZE
#error "FBDICT_DATA_SIZE is undefined"
#endif
/* Zero clear unused area after operation */
#ifndef FBDICT_ZERO_CLEAR
#error "FBDICT_ZERO_CLEAR is undefined"
#endif

#ifdef FBDICT_BIG_ENDIAN
#define __bd16			__be16
#define __bd32			__be32
#define __bd64			__be32
#define cpu_to_bd16(x)		cpu_to_be16(x)
#define cpu_to_bd32(x)		cpu_to_be32(x)
#define cpu_to_bd64(x)		cpu_to_be64(x)
#define bd16_to_cpu(x)		be16_to_cpu(x)
#define bd32_to_cpu(x)		be32_to_cpu(x)
#define bd64_to_cpu(x)		be64_to_cpu(x)
#else
#define __bd16			__le16
#define __bd32			__le32
#define __bd64			__le32
#define cpu_to_bd16(x)		cpu_to_le16(x)
#define cpu_to_bd32(x)		cpu_to_le32(x)
#define cpu_to_bd64(x)		cpu_to_le64(x)
#define bd16_to_cpu(x)		le16_to_cpu(x)
#define bd32_to_cpu(x)		le32_to_cpu(x)
#define bd64_to_cpu(x)		le64_to_cpu(x)
#endif

struct fbdict_head {
	__bd16 count;
	u8 data[];
};

/* Maximum number of entries on bd_size */
static inline int fbdict_max_count(int bd_size)
{
	return (bd_size - sizeof(struct fbdict_head))
		/ (FBDICT_KEY_SIZE + FBDICT_DATA_SIZE);
}

/* Get number of entries in block. */
static inline u16 fbdict_count(void *bd)
{
	struct fbdict_head *head = bd;
	return bd16_to_cpu(head->count);
}

/* Get size that is used. */
static inline int fbdict_used_size(void *bd)
{
	return sizeof(struct fbdict_head)
		+ (fbdict_count(bd) * (FBDICT_KEY_SIZE + FBDICT_DATA_SIZE));
}

/* Set number of entries in block. */
static inline void fbdict_set_count(void *bd, int count)
{
	struct fbdict_head *head = bd;
	head->count = cpu_to_bd16(count);
}

/* Get pointer of key for idx. */
static inline void *fbdict_keyp(void *bd, int idx)
{
	struct fbdict_head *head = bd;
	return head->data + (idx * FBDICT_KEY_SIZE);
}

/* Get data_top */
static inline void *fbdict_top_data(struct fbdict_head *head, int bd_size)
{
	return (void *)head + bd_size;
}

/* Get data_limit */
static inline void *fbdict_limit_data(struct fbdict_head *head, int bd_size)
{
	int bd_count = fbdict_count(head);
	return fbdict_top_data(head, bd_size) - (bd_count * FBDICT_DATA_SIZE);
}

/* Get pointer to data for idx. */
static inline void *fbdict_datap(void *bd, int bd_size, int idx)
{
	return fbdict_top_data(bd, bd_size) - ((idx + 1) * FBDICT_DATA_SIZE);
}

static void fbdict_init(void *bd, int bd_size)
{
	struct fbdict_head *head = bd;

	*head = (struct fbdict_head){
		.count = 0,
	};
	if (FBDICT_ZERO_CLEAR)
		memset((void *)head + sizeof(*head), 0, bd_size-sizeof(*head));
}

#ifdef FBDICT_COMPARE
#define BSEARCH_NAME		fbdict_bsearch
#define BSEARCH_COMPARE		FBDICT_COMPARE
#define BSEARCH_ENT_SIZE	FBDICT_KEY_SIZE
#include "bsearch.c"

/*
 * Search idx <= key.
 *
 * return value:
 * >= 0 - closest idx
 *   -1 - all idx are larger than key, or istart > bd_count - 1
 */
static int fbdict_lookup(void *bd, const void *key, const int istart)
{
	struct fbdict_head *head = bd;
	int bd_count = fbdict_count(head);
	return fbdict_bsearch(head->data, bd_count, key, istart);
}
#endif

/*
 * Insert key before idx, and make space for data.
 *
 * return value:
 * pointer - data that caller will fill
 * NULL    - no space to store
 */
static void *fbdict_insert(void *bd, int bd_size, int max_count,
			   int idx, void *key)
{
	struct fbdict_head *head = bd;
	u16 bd_count = fbdict_count(head);
	void *src, *dst, *data_ptr = NULL;
	int move_size;

	BUG_ON(idx > bd_count);

	if (bd_count >= max_count)
		return NULL;

	move_size = (bd_count - idx) * FBDICT_KEY_SIZE;
	src = head->data + (idx * FBDICT_KEY_SIZE);
	dst = src + FBDICT_KEY_SIZE;
	memmove(dst, src, move_size);
	memcpy(src, key, FBDICT_KEY_SIZE);

	if (FBDICT_DATA_SIZE) {
		move_size = (bd_count - idx) * FBDICT_DATA_SIZE;
		src = fbdict_limit_data(head, bd_size);
		dst = src - FBDICT_DATA_SIZE;
		memmove(dst, src, move_size);

		data_ptr = dst + move_size;
	}

	fbdict_set_count(head, bd_count + 1);

	return data_ptr;
}

/* Delete idx of key and data. */
static void fbdict_delete(void *bd, int bd_size, int idx, int nr)
{
	struct fbdict_head *head = bd;
	u16 bd_count = fbdict_count(head);
	void *src, *dst;
	int move_size;

	if (idx >= bd_count)
		return;
	nr = min(bd_count - idx, nr);

	move_size = (bd_count - (idx + nr)) * FBDICT_KEY_SIZE;
	src = head->data + ((idx + nr) * FBDICT_KEY_SIZE);
	dst = src - (nr * FBDICT_KEY_SIZE);
	memmove(dst, src, move_size);
	if (FBDICT_ZERO_CLEAR)
		memset(dst + move_size, 0, nr * FBDICT_KEY_SIZE);

	if (FBDICT_DATA_SIZE) {
		move_size = (bd_count - (idx + nr)) * FBDICT_DATA_SIZE;
		src = fbdict_limit_data(head, bd_size);
		dst = src + (nr * FBDICT_DATA_SIZE);
		memmove(dst, src, move_size);
		if (FBDICT_ZERO_CLEAR)
			memset(src, 0, nr * FBDICT_DATA_SIZE);
	}

	fbdict_set_count(head, bd_count - nr);
}

#ifdef FBDICT_NEED_SPLIT
/* Split "bfrom" at split_idx to "binto". */
static void fbdict_split(void *bfrom, int bd_size, int split_idx, void *binto)
{
	struct fbdict_head *from = bfrom;
	struct fbdict_head *into = binto;
	u16 copy_count, from_count = fbdict_count(from);
	void *src, *dst;
	int copy_size;

	fbdict_init(into, bd_size);

	if (from_count == 0 || split_idx >= from_count) {
		/* Nothing data to copy */
		return;
	}

	copy_count = from_count - split_idx;

	copy_size = copy_count * FBDICT_KEY_SIZE;
	src = from->data + (split_idx * FBDICT_KEY_SIZE);
	dst = into->data;
	memcpy(dst, src, copy_size);
	if (FBDICT_ZERO_CLEAR)
		memset(src, 0, copy_size);

	if (FBDICT_DATA_SIZE) {
		copy_size = copy_count * FBDICT_DATA_SIZE;
		src = fbdict_limit_data(from, bd_size);
		dst = fbdict_top_data(into, bd_size) - copy_size;
		memcpy(dst, src, copy_size);
		if (FBDICT_ZERO_CLEAR)
			memset(src, 0, copy_size);
	}

	fbdict_set_count(from, split_idx);
	fbdict_set_count(into, copy_count);
}
#endif

#ifdef FBDICT_NEED_MERGE
/*
 * Merge "bfrom" into "binto".
 *
 * return value:
 * true  - success
 * false - no space to store
 */
static bool fbdict_merge(void *binto, int bd_size, int max_count, void *bfrom)
{
	struct fbdict_head *from = bfrom, *into = binto;
	int from_count = fbdict_count(from);
	int into_count = fbdict_count(into);
	void *src, *dst;
	int copy_size;

	if (from_count + into_count > max_count)
		return false;

	copy_size = from_count * FBDICT_KEY_SIZE;
	src = from->data;
	dst = into->data + (into_count * FBDICT_KEY_SIZE);
	memcpy(dst, src, copy_size);

	if (FBDICT_DATA_SIZE) {
		copy_size = from_count * FBDICT_DATA_SIZE;
		src = fbdict_limit_data(from, bd_size);
		dst = fbdict_limit_data(into, bd_size) - copy_size;
		memcpy(dst, src, copy_size);
	}

	fbdict_set_count(into, into_count + from_count);

	//fbdict_init(bfrom, bd_size);

	return true;
}
#endif

#undef FBDICT_LITTLE_ENDIAN
#undef FBDICT_BIG_ENDIAN
#undef FBDICT_KEY_SIZE
#undef FBDICT_DATA_SIZE
#undef FBDICT_COMPARE
#undef FBDICT_ZERO_CLEAR
#undef __bd16
#undef __bd32
#undef __bd64
#undef cpu_to_bd16
#undef cpu_to_bd32
#undef cpu_to_bd64
#undef bd16_to_cpu
#undef bd32_to_cpu
#undef bd64_to_cpu
