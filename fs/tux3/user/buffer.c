#include <stdlib.h>
#include <stddef.h>
#include <errno.h>
#ifndef TUX3_BUILD
#include "diskio.h"
#endif
#include "buffer.h"
#include "trace.h"
#include "libklib/err.h"
#include "libklib/list_sort.h"
#include "fault_inject.h"

#define buftrace trace_off

/*
 * Emulate kernel buffers in userspace
 *
 * Even though we are in user space, for reasons of durability and speed
 * we need to access the block directly, handle our own block caching and
 * keep track block by block of which parts of the on-disk data structures
 * as they are accessed and modified.  There's no need to reinvent the
 * wheel here.  I have basically cloned the traditional Unix kernel buffer
 * paradigm, with one small twist of my own, that is, instead of state
 * bits we use scalar values.  This captures the notion of buffer state
 * transitions more precisely than the traditional approach.
 *
 * One big benefit of using a buffer paradigm that looks and acts very
 * much like the kernel incarnation is, porting this into the kernel is
 * going to be a whole lot easier.  Most higher level code will not need
 * to be modified at all.  Another benefit is, it will be much easier to
 * add async IO.
 */

#define BUF_ALIGN_BITS		9
#define BUF_ALIGN_SIZE		(1 << BUF_ALIGN_BITS)
#define MAX_BUFFERS_MIN		100U

#ifdef BUFFER_PARANOIA_DEBUG
/*
 * 0 - no debug
 * 1 - leak check
 * 2 - "1" and reclaim buffer early
 */
static int debug_level;
#endif

static struct list_head buffers[BUFFER_STATES], lru_buffers;
static unsigned buffer_poolsize, buffer_blocksize;
static unsigned max_buffers, max_evict, buffer_count;

void show_buffer(struct buffer_head *buffer)
{
	printf("%Lx/%i%s ", buffer->index, buffer->count,
		buffer_dirty(buffer) ? "*" :
		buffer_clean(buffer) ? "" :
		buffer->state == BUFFER_EMPTY ? "-" :
		"?");
}

void show_buffers_(map_t *map, int all)
{
	struct buffer_head *buffer;
	unsigned i;

	for (i = 0; i < BUFFER_BUCKETS; i++) {
		struct hlist_head *bucket = &map->hash[i];
		if (hlist_empty(bucket))
			continue;

		printf("[%i] ", i);
		hlist_for_each_entry(buffer, bucket, hashlink) {
			if (all || buffer->count >= !hlist_unhashed(&buffer->hashlink) + 1)
				show_buffer(buffer);
		}
		printf("\n");
	}
}

void show_active_buffers(map_t *map)
{
	printf("(map %p)\n", map);
	show_buffers_(map, 0);
}

void show_buffers(map_t *map)
{
	printf("(map %p)\n", map);
	show_buffers_(map, 1);
}

void show_buffer_list(struct list_head *list)
{
	struct buffer_head *buffer;
	unsigned count = 0;
	list_for_each_entry(buffer, list, link) {
		show_buffer(buffer);
		count++;
	}
	printf("(%i)\n", count);
}

void show_dirty_buffers(map_t *map)
{
	for (int i = 0; i < BUFFER_DIRTY_STATES; i++) {
		printf("map %p dirty [%d]: ", map, i);
		show_buffer_list(tux3_dirty_buffers(map->inode, i));
	}
}

void show_buffers_state(unsigned state)
{
	printf("buffers in state %u: ", state);
	show_buffer_list(buffers + state);
}

int count_buffers(void)
{
	struct buffer_head *safe, *buffer;
	int count = 0;
	list_for_each_entry_safe(buffer, safe, &lru_buffers, lru) {
		if (buffer->count <= !hlist_unhashed(&buffer->hashlink))
			continue;
		trace_off("buffer %Lx has non-zero count %d", (long long)buffer->index, buffer->count);
		count++;
	}
	return count;
}

static int reclaim_buffer(struct buffer_head *buffer)
{
	/* If buffer is not dirty and ->count == 1, we can reclaim buffer */
	if (buffer->count == 1 && !buffer_dirty(buffer)) {
		if (!hlist_unhashed(&buffer->hashlink)) {
			remove_buffer_hash(buffer);
			return 1;
		}
	}
	return 0;
}

static inline int reclaim_buffer_early(struct buffer_head *buffer)
{
#ifdef BUFFER_PARANOIA_DEBUG
	if (debug_level >= 2)
		return reclaim_buffer(buffer);
#endif
	return 0;
}

static inline int is_reclaim_buffer_early(void)
{
#ifdef BUFFER_PARANOIA_DEBUG
	if (debug_level >= 2)
		return 1;
#endif
	return 0;
}

int set_buffer_state_list(struct buffer_head *buffer, unsigned state, struct list_head *list)
{
	if (buffer->state != state) {
		list_move_tail(&buffer->link, list);
		buffer->state = state;
		/* state was changed, try to reclaim */
		reclaim_buffer_early(buffer);
		return 1;
	}
	return 0;
}

static inline void set_buffer_state(struct buffer_head *buffer, unsigned state)
{
	set_buffer_state_list(buffer, state, buffers + state);
}

int tux3_set_buffer_dirty_list(map_t *map, struct buffer_head *buffer,
				int delta, struct list_head *head)
{
	return set_buffer_state_list(buffer, tux3_bufsta_delta(delta), head);
}

int tux3_set_buffer_dirty(map_t *map, struct buffer_head *buffer, int delta)
{
	struct list_head *head = tux3_dirty_buffers(map->inode, delta);
	return tux3_set_buffer_dirty_list(map, buffer, delta, head);
}

#ifndef TUX3_BUILD
struct buffer_head *set_buffer_dirty(struct buffer_head *buffer)
{
	tux3_set_buffer_dirty(buffer->map, buffer, BUFFER_INIT_DELTA);
	return buffer;
}
#endif

struct buffer_head *set_buffer_clean(struct buffer_head *buffer)
{
	assert(!buffer_clean(buffer));
	set_buffer_state(buffer, BUFFER_CLEAN);
	return buffer;
}

struct buffer_head *__set_buffer_empty(struct buffer_head *buffer)
{
	set_buffer_state(buffer, BUFFER_EMPTY);
	return buffer;
}

struct buffer_head *set_buffer_empty(struct buffer_head *buffer)
{
	assert(!buffer_empty(buffer));
	return __set_buffer_empty(buffer);
}

void tux3_clear_buffer_dirty(struct buffer_head *buffer, unsigned delta)
{
#ifdef TUX3_BUILD
	assert(buffer_can_modify(buffer, delta));
#endif
	/* FIXME: this should be set_buffer_empty()? */
	set_buffer_clean(buffer);
}

/* Cleanup dirty for I/O path (for volmap/logmap) */
static void __clear_buffer_dirty_for_endio(struct buffer_head *buffer, int err)
{
	if (err) {
		/* FIXME: What to do? Hack: This re-link to state from bufvec */
		assert(0);
		__set_buffer_empty(buffer);
	} else {
		/* FIXME: this should be set_buffer_empty()? */
		set_buffer_clean(buffer);
	}
}

/* Cleanup dirty for I/O path (for filemap) */
void clear_buffer_dirty_for_endio(struct buffer_head *buffer, int err)
{
	int forked = hlist_unhashed(&buffer->hashlink);

	__clear_buffer_dirty_for_endio(buffer, err);

	/* Is this forked buffer? */
	if (forked) {
		/* We have to unpin forked buffer to free. See blockdirty() */
		blockput(buffer);
	}
}

#ifdef BUFFER_PARANOIA_DEBUG
static void __free_buffer(struct buffer_head *buffer)
{
	list_del(&buffer->link);
	free(buffer->data);
	free(buffer);
}
#endif

static void free_buffer(struct buffer_head *buffer)
{
#ifdef BUFFER_PARANOIA_DEBUG
	if (debug_level) {
		__free_buffer(buffer);
		buffer_count--;
		return;
	}
#endif
	/* insert at head, not tail? */
	set_buffer_state(buffer, BUFFER_FREED);
	buffer->map = NULL;
	buffer_count--;
}

void blockput(struct buffer_head *buffer)
{
	assert(buffer);
	assert(buffer->count > 0);
	buftrace("Release buffer %Lx, count = %i, state = %i", buffer->index, buffer->count, buffer->state);
	buffer->count--;
	if (buffer->count == 0) {
		buftrace("Free buffer %Lx", buffer->index);
		assert(!buffer_dirty(buffer));
		assert(hlist_unhashed(&buffer->hashlink));
		assert(list_empty(&buffer->lru));
		free_buffer(buffer);
		return;
	}

	reclaim_buffer_early(buffer);
}

void get_bh(struct buffer_head *buffer)
{
	assert(buffer->count >= 1);
	buffer->count++;
}

/* This is called for the freeing block on volmap */
static void __blockput_free(struct buffer_head *buffer, unsigned delta)
{
	if (bufcount(buffer) != 2) { /* caller + hashlink == 2 */
		printf("Error: free block %Lx/%x still in use!\n",
		       bufindex(buffer), bufcount(buffer));
		blockput(buffer);
		assert(bufcount(buffer) == 1);
		return;
	}
	/* free it!!! (and need a buffer free state) */
	tux3_clear_buffer_dirty(buffer, delta);
	blockput(buffer);
}

void blockput_free(struct sb *sb, struct buffer_head *buffer)
{
	__blockput_free(buffer, BUFFER_INIT_DELTA);
}

void blockput_free_unify(struct sb *sb, struct buffer_head *buffer)
{
	__blockput_free(buffer, sb->unify);
}

unsigned buffer_hash(block_t block)
{
	return (((block >> 32) ^ (block_t)block) * 978317583) % BUFFER_BUCKETS;
}

void insert_buffer_hash(struct buffer_head *buffer)
{
	map_t *map = buffer->map;
	struct hlist_head *bucket = map->hash + buffer_hash(buffer->index);
	get_bh(buffer); /* get additonal refcount for hashlink */
	hlist_add_head(&buffer->hashlink, bucket);
	list_add_tail(&buffer->lru, &lru_buffers);
}

void remove_buffer_hash(struct buffer_head *buffer)
{
	list_del_init(&buffer->lru);
	hlist_del_init(&buffer->hashlink);
	blockput(buffer); /* put additonal refcount for hashlink */
}

static void evict_buffer(struct buffer_head *buffer)
{
	buftrace("evict buffer [%Lx]", buffer->index);
	assert(buffer_clean(buffer) || buffer_empty(buffer));
	assert(buffer->count == 1);
	reclaim_buffer(buffer);
}

static void try_reclaim_buffers(unsigned max_reclaims)
{
	struct buffer_head *safe, *victim;
	int count = 0;

	list_for_each_entry_safe(victim, safe, &lru_buffers, lru) {
		if (reclaim_buffer(victim)) {
			if (++count == max_reclaims)
				break;
		}
	}
}

struct buffer_head *new_buffer(map_t *map)
{
	struct buffer_head *buffer = NULL;
	struct list_head *freed_list = &buffers[BUFFER_FREED];
	int nofail = !!(mapping_gfp_mask(map) & __GFP_NOFAIL);
	int err;

	if (!nofail)
		fault_return("io:memory:", ERR_PTR(-ENOMEM));

	if (!list_empty(freed_list)) {
		buffer = list_entry(freed_list->next, struct buffer_head, link);
		goto have_buffer;
	}

	if (buffer_count >= max_buffers) {
		buftrace("try to evict buffers");
		try_reclaim_buffers(max_evict);

		if (!list_empty(freed_list)) {
			buffer = list_entry(freed_list->next, struct buffer_head, link);
			goto have_buffer;
		}
	}

	buftrace("expand buffer pool");
	if (buffer_count == max_buffers) {
		printf("Warning: maximum buffer count exceeded (%i)\n",
		       buffer_count);
		if (nofail)
			goto error_nofail;
		return ERR_PTR(-ENOMEM);
	}

	if (buffer_blocksize != 1 << map->dev->bits)
		return ERR_PTR(-EINVAL);

	fault_disable_injection();
	buffer = malloc(sizeof(struct buffer_head));
	fault_enable_injection();
	if (!buffer) {
		if (nofail)
			goto error_nofail;
		return ERR_PTR(-ENOMEM);
	}
	*buffer = (struct buffer_head){
		.state	= BUFFER_FREED,
		.link	= LIST_HEAD_INIT(buffer->link),
		.lru	= LIST_HEAD_INIT(buffer->lru),
	};
	INIT_HLIST_NODE(&buffer->hashlink);

	fault_disable_injection();
	err = posix_memalign(&buffer->data, BUF_ALIGN_SIZE, buffer_blocksize);
	fault_enable_injection();
	if (err) {
		printf("Error: unable to expand buffer pool: %s\n",
		       strerror(err));
		free(buffer);
		if (nofail)
			goto error_nofail;
		return ERR_PTR(-err);
	}

have_buffer:
	assert(buffer->count == 0);
	assert(buffer->state == BUFFER_FREED);
	buffer->map = map;
	buffer->count = 1;
	set_buffer_empty(buffer);
	buffer_count++;
	return buffer;

error_nofail:
	printf("Error: failed memory allocation with __GFP_NOFAIL\n");
	exit(1);
}

struct buffer_head *peekblk(map_t *map, block_t block)
{
	struct hlist_head *bucket = map->hash + buffer_hash(block);
	struct buffer_head *buffer;

	hlist_for_each_entry(buffer, bucket, hashlink) {
		if (buffer->index == block) {
			get_bh(buffer);
			return buffer;
		}
	}
	return NULL;
}

struct buffer_head *blockget(map_t *map, block_t block)
{
	struct hlist_head *bucket = map->hash + buffer_hash(block);
	struct buffer_head *buffer;

	hlist_for_each_entry(buffer, bucket, hashlink) {
		if (buffer->index == block) {
			list_move_tail(&buffer->lru, &lru_buffers);
			get_bh(buffer);
			return buffer;
		}
	}

	buftrace("make buffer [%Lx]", block);
	buffer = new_buffer(map);
	if (IS_ERR(buffer))
		return NULL; // ERR_PTR me!!!
	buffer->index = block;
	insert_buffer_hash(buffer);
	return buffer;
}

struct buffer_head *blockread(map_t *map, block_t block)
{
	struct buffer_head *buffer = blockget(map, block);
	if (buffer && buffer_empty(buffer)) {
		struct bufvec bufvec;
		int ret;

		bufvec_init(&bufvec, REQ_OP_READ, 0, map, NULL, NULL);
		ret = bufvec_contig_add(&bufvec, buffer);
		assert(ret == 1);

		buftrace("read buffer [%Lx], state %i", buffer->index, buffer->state);
		int err = buffer->map->io(&bufvec);
		if (err || !buffer_clean(buffer)) {
			blockput(buffer);
			return NULL; // ERR_PTR me!!!
		}
	}
	return buffer;
}

/* Invalidate check, this must be called from frontend like truncate */
static void tux3_invalidate_check(struct buffer_head *buffer)
{
#ifdef TUX3_BUILD
	unsigned delta = tux3_inode_delta(buffer->map->inode);
	assert(buffer_can_modify(buffer, delta));
#endif
}

static void invalidate_buffer(struct buffer_head *buffer, int check)
{
	if (!buffer_empty(buffer)) {
		if (check)
			tux3_invalidate_check(buffer);
		set_buffer_empty(buffer);
	}
	if (!is_reclaim_buffer_early())
		reclaim_buffer(buffer);
}

void truncate_buffers_range(map_t *map, loff_t lstart, loff_t lend)
{
	unsigned blockbits = map->dev->bits;
	unsigned blocksize = 1 << blockbits;
	block_t start = (lstart + blocksize - 1) >> blockbits;
	block_t end = lend >> blockbits;
	unsigned partial = lstart & (blocksize - 1);
	unsigned partial_size = blocksize - partial;
	unsigned i;

	assert((lend & (blocksize - 1)) == (blocksize - 1));

	for (i = 0; i < BUFFER_BUCKETS; i++) {
		struct hlist_head *bucket = &map->hash[i];
		struct buffer_head *buffer;
		struct hlist_node *n;

		hlist_for_each_entry_safe(buffer, n, bucket, hashlink) {
			/* Clear partial truncated buffer */
			if (partial && buffer->index == start - 1)
				memset(buffer->data + partial, 0, partial_size);

			if (buffer->index < start || end < buffer->index)
				continue;

			/* Do buffer fork to invalidate */
			if (bufferfork_to_invalidate(map, buffer))
				continue;

			/* Invalidate buffers */
			invalidate_buffer(buffer, 1);
		}
	}
}

/* !!! only used for testing */
void invalidate_buffers(map_t *map)
{
	unsigned i;
	for (i = 0; i < BUFFER_BUCKETS; i++) {
		struct hlist_head *bucket = &map->hash[i];
		struct buffer_head *buffer;
		struct hlist_node *n;

		hlist_for_each_entry_safe(buffer, n, bucket, hashlink) {
			if (buffer->count == 1)
				invalidate_buffer(buffer, 0);
		}
	}
}

#ifdef BUFFER_PARANOIA_DEBUG
static void free_state_buffers(int state)
{
	struct list_head *head = buffers + state;
	struct buffer_head *buffer, *safe;

	list_for_each_entry_safe(buffer, safe, head, link) {
		list_del(&buffer->lru);
		__free_buffer(buffer);
	}
}

static void destroy_buffers(void)
{
	struct buffer_head *buffer, *safe;

	/* If debug_buffer, buffer should already be freed */

	for (int i = 0; i < BUFFER_STATES; i++) {
		struct list_head *head = buffers + i;

		if (!debug_level)
			free_state_buffers(i);

		if (!list_empty(head)) {
			printf("Error: state %d: buffer leak, or list corruption?\n", i);
			list_for_each_entry(buffer, head, link) {
				printf("map [%p] ", buffer->map);
				show_buffer(buffer);
			}
			printf("\n");
		}
		assert(list_empty(head));
	}

	/*
	 * If buffer is dirty, it may not be on buffers state list
	 * (e.g. buffer may be on map->dirty).
	 */
	if (!debug_level) {
		list_for_each_entry_safe(buffer, safe, &lru_buffers, lru) {
			assert(buffer_dirty(buffer));
			list_del(&buffer->lru);
			__free_buffer(buffer);
		}
	}
	if (!list_empty(&lru_buffers)) {
		printf("Error: dirty buffer leak, or list corruption?\n");
		list_for_each_entry(buffer, &lru_buffers, lru) {
			if (buffer_dirty(buffer)) {
				printf("map [%p] ", buffer->map);
				show_buffer(buffer);
			}
		}
		printf("\n");
		assert(list_empty(&lru_buffers));
	}
}

static int preallocate_buffers(unsigned bufsize)
{
	return 0;
}

static void free_old_buffers(void)
{
	free_state_buffers(BUFFER_FREED);
}
#else /* !BUFFER_PARANOIA_DEBUG */

static struct buffer_head *prealloc_heads;
static void *data_pool;

static int preallocate_buffers(unsigned bufsize)
{
	int i, err;

	buftrace("Pre-allocating buffers...");
	prealloc_heads = malloc(max_buffers * sizeof(*prealloc_heads));
	if (!prealloc_heads) {
		printf("Warning: unable to pre-allocate buffers."
		       " Using on demand allocation for buffers\n");
		err = -ENOMEM;
		goto error;
	}

	buftrace("Pre-allocating data for buffers...");
	err = posix_memalign(&data_pool, BUF_ALIGN_SIZE, max_buffers * bufsize);
	if (err) {
		printf("Error: unable to allocate space for buffer data: %s\n",
		       strerror(err));
		err = -err;
		goto error_memalign;
	}

	//memset(data_pool, 0xdd, max_buffers*bufsize); /* first time init to deadly data */
	for (i = 0; i < max_buffers; i++) {
		prealloc_heads[i] = (struct buffer_head){
			.data	= data_pool + i*bufsize,
			.state	= BUFFER_FREED,
			.lru	= LIST_HEAD_INIT(prealloc_heads[i].lru),
		};
		INIT_HLIST_NODE(&prealloc_heads[i].hashlink);

		list_add_tail(&prealloc_heads[i].link, buffers + BUFFER_FREED);
	}

	return 0; /* sucess on pre-allocation of buffers */

error_memalign:
	free(prealloc_heads);
error:
	return err;
}

static void free_old_buffers(void)
{
	if (prealloc_heads)
		free(prealloc_heads);
	if (data_pool)
		free(data_pool);

	INIT_LIST_HEAD(&buffers[BUFFER_FREED]);
}
#endif /* !BUFFER_PARANOIA_DEBUG */

/* Note, this doesn't invalidate dirty buffers unlike kernel. */
int set_blocksize(unsigned blocksize)
{
	if (blocksize == 0)
		return -EINVAL;
	if (blocksize == buffer_blocksize)
		return 0;

	/* Try reclaim all buffers */
	try_reclaim_buffers(buffer_count);
	/* All buffer is BUFFER_FREED? */
	if (buffer_count != 0)
		return -EBUSY;

	/* Free old buffers */
	free_old_buffers();

	/* Initialize buffers */
	buffer_blocksize = blocksize;
	max_buffers = max(buffer_poolsize / buffer_blocksize, MAX_BUFFERS_MIN);
	max_evict = max_buffers / 10;

	return preallocate_buffers(buffer_blocksize);
}

void init_buffer_params(unsigned poolsize, int debug)
{
	buffer_poolsize = poolsize;

	INIT_LIST_HEAD(&lru_buffers);
	for (int i = 0; i < BUFFER_STATES; i++)
		INIT_LIST_HEAD(buffers + i);

#ifdef BUFFER_PARANOIA_DEBUG
	debug_level = debug;
	atexit(destroy_buffers);
#endif
}

int __tux3_volmap_io(struct bufvec *bufvec, block_t block, unsigned count)
{
	assert(bufvec_contig_buf(bufvec)->map->dev->bits >= 6 &&
	       bufvec_contig_buf(bufvec)->map->dev->fd);

	bufvec->end_io = __clear_buffer_dirty_for_endio;

	return blockio_vec(bufvec, block, count);
}

static int dev_blockio(struct bufvec *bufvec)
{
	block_t block = bufvec_contig_index(bufvec);
	unsigned count = bufvec_contig_count(bufvec);

	return __tux3_volmap_io(bufvec, block, count);
}

int dev_errio(struct bufvec *bufvec)
{
	assert(0);
	return -EIO;
}

map_t *new_map(struct dev *dev, blockio_t *io)
{
	map_t *map = malloc(sizeof(*map));
	if (map) {
		*map = (map_t){
			.dev	= dev,
			.io	= io ? io : dev_blockio
		};
		for (int i = 0; i < BUFFER_BUCKETS; i++)
			INIT_HLIST_HEAD(&map->hash[i]);
	}
	return map;
}

void free_map(map_t *map)
{
	for (int i = 0; i < BUFFER_BUCKETS; i++) {
		struct hlist_head *bucket = &map->hash[i];
		struct buffer_head *buffer;
		struct hlist_node *n;

		hlist_for_each_entry_safe(buffer, n, bucket, hashlink)
			evict_buffer(buffer);
	}
	free(map);
}

#include "buffer_writeback.c"
#include "buffer_fork.c"
