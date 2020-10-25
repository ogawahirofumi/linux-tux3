/*
 * Single linked list support (LIFO/FIFO order).
 *
 * Copyright (c) 2008-2014 Daniel Phillips
 * Copyright (c) 2008-2014 OGAWA Hirofumi
 */

#ifndef TUX3_LINK_H
#define TUX3_LINK_H

#define LINK_POISON1  ((void *) 0x00300400)

/* Single linked list (LIFO order) */

struct link { struct link *next; };

#define LINK_INIT_CIRCULAR(name)	{ &(name), }
#define link_entry(ptr, type, member)	container_of(ptr, type, member)
/* take care: this doesn't check member is `struct link *' or not */
#define __link_entry(ptr, type, member)					\
	container_of((typeof(((type *)0)->member) *)ptr, type, member)

static inline void init_link_circular(struct link *head)
{
	head->next = head;
}

static inline int link_empty(const struct link *head)
{
	return head->next == head;
}

static inline void link_add(struct link *node, struct link *head)
{
	node->next = head->next;
	head->next = node;
}

static inline void link_del_next(struct link *node)
{
	struct link *deleted = node->next;

	node->next = node->next->next;

	if (deleted->next != deleted)
		deleted->next = LINK_POISON1;
}

#define link_for_each(pos, head)					\
	for (pos = (head)->next; pos != (head); pos = pos->next)

/* this can call list_del_next() while looping. */
#define link_for_each_safe(pos, prev, n, head)				\
	for (pos = (head)->next, prev = (head), n = pos->next;		\
	     pos != (head);						\
	     prev = ((prev->next == n) ? prev : pos), pos = n, n = pos->next)


/*
 * Single linked list (FIFO order)
 *
 *         head->tail --+
 *                      V
 *   +----------->  [  tail ]->next  -----------------+
 *   |                                                |
 *   +---  next<-[  node ] <---  next<-[ front ]  <---+
 */

struct flink_head { struct link *tail; };

#define FLINK_HEAD_INIT(name)	{ NULL, }
#define flink_entry(ptr, type, member)					\
	link_entry(ptr, type, member)
/* take care: this doesn't check member is `struct link *' or not */
#define __flink_entry(ptr, type, member)				\
	__link_entry(ptr, type, member)
#define flink_front_entry(head, type, member)				\
	flink_entry(flink_front(head), type, member)
/* take care: this doesn't check member is `struct link *' or not */
#define __flink_front_entry(head, type, member) ({			\
	struct link *__next = flink_front(head);			\
	__flink_entry(__next, type, member);				\
})

static inline void init_flink_head(struct flink_head *head)
{
	head->tail = NULL;
}

static inline int flink_empty(const struct flink_head *head)
{
	return head->tail == NULL;
}

/* Tests whether a flink has just one entry. */
static inline int flink_is_singular(const struct flink_head *head)
{
	return !flink_empty(head) && link_empty(head->tail);
}

static inline struct link *flink_front(const struct flink_head *head)
{
	return head->tail->next;
}

/* Add a first node. The head must be empty. */
static inline void flink_first_add(struct link *node, struct flink_head *head)
{
	assert(flink_empty(head));
	init_link_circular(node);
	head->tail = node;
}

/*
 * Insert a node after the specified pos.
 *
 * If pos == tail, a node will be added as front.
 */
static inline void __flink_add_next(struct link *node, struct link *pos)
{
	link_add(node, pos);
}

/* Insert a node at front. The head must not be empty. */
static inline void __flink_add_front(struct link *node, struct flink_head *head)
{
	__flink_add_next(node, head->tail);
}

/* Insert a node at front. */
static inline void flink_add_front(struct link *node, struct flink_head *head)
{
	if (flink_empty(head))
		flink_first_add(node, head);
	else
		__flink_add_front(node, head);
}

/* Insert a node at tail. The head must not be empty. */
static inline void __flink_add_tail(struct link *node, struct flink_head *head)
{
	__flink_add_front(node, head);
	head->tail = node;
}

/* Insert a node at tail. */
static inline void flink_add_tail(struct link *node, struct flink_head *head)
{
	if (flink_empty(head))
		flink_first_add(node, head);
	else
		__flink_add_tail(node, head);
}

/*
 * Insert a node after the specified pos. The head must not be empty.
 *
 * If pos == tail, a node will be added as tail.
 */
static inline void flink_add_next(struct link *node, struct link *pos,
				  struct flink_head *head)
{
	if (pos == head->tail)
		__flink_add_tail(node, head);
	else
		__flink_add_next(node, pos);
}

/* Delete a last node. */
static inline void flink_del_singular(struct flink_head *head)
{
	assert(flink_is_singular(head));
	head->tail->next = LINK_POISON1;
	init_flink_head(head);
}

/*
 * Delete a next node of specified pos. The head must not be empty.
 *
 * This doesn't delete a last node. To delete a last node, it has to
 * use flink_del_singular() instead.
 *
 * Note: If (!flink_is_singular(head) && pos->next == head->tail),
 * this breaks head->tail link, so caller must take care it.
 */
static inline void __flink_del_next(struct link *pos, struct flink_head *head)
{
	link_del_next(pos);
}

/* Delete a next node of specified pos. The head must not be empty. */
static inline void flink_del_next(struct link *pos, struct flink_head *head)
{
	if (flink_is_singular(head))
		flink_del_singular(head);
	else {
		/* If deleted a tail node, a pos becomes a tail */
		if (pos->next == head->tail)
			head->tail = pos;
		__flink_del_next(pos, head);
	}
}

/*
 * Delete a node at front. The head must not be empty.
 *
 * This doesn't delete a last node. To delete a last node, it has to
 * use flink_del_singular() instead.
 */
static inline void __flink_del_front(struct flink_head *head)
{
	__flink_del_next(head->tail, head);
}

/* Delete a node at front. The head must not be empty. */
static inline void flink_del_front(struct flink_head *head)
{
	if (flink_is_singular(head))
		flink_del_singular(head);
	else
		__flink_del_front(head);
}

#define flink_for_each(pos, n, head)					\
	if (!flink_empty(head))						\
		for (pos = (head)->tail->next, n = NULL;		\
		     n == NULL || n->next != (head)->tail->next;	\
		     n = pos, pos = pos->next)

/*
 * This can call flink_del_next(prev) while looping.
 *
 * Note, this can delete prev->next only. If other position was
 * deleted, undefined behavior. (Especially, head->tail should not be
 * changed.)
 */
#define flink_for_each_safe(pos, prev, n, head)				\
	if (!flink_empty(head))						\
		for (pos = (head)->tail->next,				\
			     prev = (head)->tail, n = NULL;		\
		     !flink_empty(head) &&				\
			     (n == NULL || prev->next != (head)->tail->next); \
		     !flink_empty(head) &&				\
			     /* pos is not deleted? */			\
			     ((prev->next == pos) ?			\
			      /* not deleted */				\
			      (n = (void *)1, prev = pos, pos = pos->next) : \
			      /* deleted, use same prev */		\
			      (pos = prev->next)))

#endif /* !TUX3_LINK_H */
