/*
 * libklib tests
 */

#include "tux3user.h"
#include "test.h"

struct node {
	int a;
	struct link link;
};

static void __link_check_entries(struct link *head, struct node *order[],
				 size_t nr_order)
{
	int i = 0;

	struct link *pos;
	link_for_each(pos, head) {
		struct node *p = link_entry(pos, struct node, link);
#if 0
		printf("%d: node %p, next %p\n", p->a,
		       &p->link, p->link.next);
#endif
		test_assert(p == order[i++]);
	}
	test_assert(i == nr_order);
}
#define link_check_entries(__h, ...)	do {				\
	struct node *__order[] = { __VA_ARGS__ };			\
	__link_check_entries(__h, __order, ARRAY_SIZE(__order));	\
} while (0)

static void test_link(void *_arg)
{
	struct link head = LINK_INIT_CIRCULAR(head);
	test_assert(link_empty(&head));

	/* link_add() test */
	struct node node1 = { .a = 1, };
	link_add(&node1.link, &head);
	link_check_entries(&head,
			   &node1);

	struct node node2 = { .a = 2, };
	link_add(&node2.link, &head);
	link_check_entries(&head,
			   &node2, &node1);

	struct node node3 = { .a = 3, };
	link_add(&node3.link, &node1.link);
	link_check_entries(&head,
			   &node2, &node1, &node3);

	struct node node4 = { .a = 4, };
	link_add(&node4.link, &head);
	link_check_entries(&head,
			   &node4, &node2, &node1, &node3);

	struct node node5 = { .a = 5, };
	link_add(&node5.link, &head);
	test_assert(!link_empty(&head));
	link_check_entries(&head,
			   &node5, &node4, &node2, &node1, &node3);

	/* link_del_next() test */
	link_del_next(&head);
	link_check_entries(&head,
			   &node4, &node2, &node1, &node3);

	/* link_for_each_safe() and delete test */
	struct node *test_order[] = {
		&node2, &node1,
	};
	int i = 0;
	struct link *pos, *prev, *n;
	link_for_each_safe(pos, prev, n, &head) {
		struct node *p = link_entry(pos, struct node, link);
		if (p->a == 4 || p->a == 3) {
			/* delete node4 and node3 */
			link_del_next(prev);
			continue;
		}
		test_assert(p == test_order[i++]);
	}
	link_check_entries(&head, &node2, &node1);
	test_assert(!link_empty(&head));

	/* delete all test */
	while (!link_empty(&head))
		link_del_next(&head);
	test_assert(link_empty(&head));
}
TEST_DEFINE(TEST_UNIT, "link", test_link);

static void __flink_check_entries(struct flink_head *head, struct node *order[],
				  size_t nr_order)
{
	int i = 0;
	struct link *pos, *n;
	flink_for_each(pos, n, head) {
		struct node *p = flink_entry(pos, struct node, link);
#if 0
		printf("%d: node %p, next %p\n", p->a,
		       &p->link, p->link.next);
#endif
		test_assert(p == order[i++]);
	}
	test_assert(i == nr_order);
}
#define flink_check_entries(__h, ...)	do {				\
	struct node *__order[] = { __VA_ARGS__ };			\
	__flink_check_entries(__h, __order, ARRAY_SIZE(__order));	\
} while (0)

static void test_flink(void *_arg)
{
	struct flink_head head = FLINK_HEAD_INIT(head);
	test_assert(flink_empty(&head));
	test_assert(!flink_is_singular(&head));

	/* {flink_add_tail,flink_add_front}() test */
	struct node node1 = { .a = 1, };
	flink_add_tail(&node1.link, &head);
	test_assert(!flink_empty(&head));
	test_assert(flink_is_singular(&head));
	flink_check_entries(&head, &node1);

	struct node node2 = { .a = 2, };
	flink_add_tail(&node2.link, &head);
	flink_check_entries(&head, &node1, &node2);

	struct node node3 = { .a = 3, };
	flink_add_front(&node3.link, &head);
	flink_check_entries(&head, &node3, &node1, &node2);

	struct node node4 = { .a = 4, };
	flink_add_front(&node4.link, &head);
	flink_check_entries(&head, &node4, &node3, &node1, &node2);

	struct node node5 = { .a = 5, };
	flink_add_tail(&node5.link, &head);
	test_assert(!flink_empty(&head));
	test_assert(!flink_is_singular(&head));
	flink_check_entries(&head, &node4, &node3, &node1, &node2, &node5);

	/* flink_del_front(), flink_add_next() test */
	flink_del_front(&head);
	flink_add_next(&node4.link, &node3.link, &head);

	flink_check_entries(&head, &node3, &node4, &node1, &node2, &node5);

	/* add tail by flink_add_next() test */
	flink_del_front(&head);
	flink_add_next(&node3.link, &node5.link, &head);
	test_assert(!flink_empty(&head));
	test_assert(!flink_is_singular(&head));

	flink_check_entries(&head, &node4, &node1, &node2, &node5, &node3);

	/* delete tail by flink_del_next() test */
	flink_del_next(&node5.link, &head);
	flink_add_front(&node3.link, &head);
	flink_check_entries(&head, &node3, &node4, &node1, &node2, &node5);

	/* make empty head test */
	while (!flink_empty(&head))
		flink_del_front(&head);
	test_assert(flink_empty(&head));

	/* flink_add_front() with empty head test */
	flink_add_front(&node5.link, &head);
	flink_check_entries(&head, &node5);
	flink_add_front(&node4.link, &head);
	flink_check_entries(&head, &node4, &node5);
	flink_add_front(&node3.link, &head);
	flink_check_entries(&head, &node3, &node4, &node5);

	/* flink_del_front() with empty head test */
	flink_del_front(&head);
	flink_check_entries(&head, &node4, &node5);
	flink_del_front(&head);
	flink_check_entries(&head, &node5);
	flink_del_front(&head);
	test_assert(flink_empty(&head));

	/* flink_add_tail() with empty head test */
	flink_add_tail(&node3.link, &head);
	flink_check_entries(&head, &node3);
	flink_add_tail(&node4.link, &head);
	flink_check_entries(&head, &node3, &node4);
	flink_add_tail(&node5.link, &head);
	flink_check_entries(&head, &node3, &node4, &node5);

	/* flink_del_next() with empty head test */
	flink_del_next(&node4.link, &head);
	flink_check_entries(&head, &node3, &node4);
	flink_del_next(&node4.link, &head);
	flink_check_entries(&head, &node4);
	flink_del_next(&node4.link, &head);
	test_assert(flink_empty(&head));

	/* flink_add_next() with empty head test */
	flink_add_front(&node3.link, &head);
	flink_check_entries(&head, &node3);
	flink_add_next(&node4.link, &node3.link, &head);
	flink_check_entries(&head, &node3, &node4);
	flink_add_next(&node5.link, &node3.link, &head);
	flink_check_entries(&head, &node3, &node5, &node4);
	flink_add_next(&node1.link, &node3.link, &head);
	flink_check_entries(&head, &node3, &node1, &node5, &node4);
	flink_add_next(&node2.link, &node3.link, &head);
	flink_check_entries(&head, &node3, &node2, &node1, &node5, &node4);

	/* flink_for_each_safe() and delete test */
	struct node *test_order[] = {
		&node2, &node1,
	};
	int i = 0;
	struct link *pos, *prev, *n;
	flink_for_each_safe(pos, prev, n, &head) {
		struct node *p = link_entry(pos, struct node, link);
		if (p->a == 3 || p->a == 5 || p->a == 4) {
			/* delete node3/node5/node4 */
			flink_del_next(prev, &head);
			continue;
		}
		test_assert(p == test_order[i++]);
	}
	flink_check_entries(&head, &node2, &node1);
	test_assert(!flink_empty(&head));

	/* flink_for_each_safe() and delete tail test */
	flink_add_front(&node3.link, &head);
	flink_check_entries(&head, &node3, &node2, &node1);
	flink_for_each_safe(pos, prev, n, &head) {
		struct node *p = link_entry(pos, struct node, link);
		if (p->a == 1) {
			flink_del_next(prev, &head);
			continue;
		}
	}
	flink_check_entries(&head, &node3, &node2);
	test_assert(!flink_empty(&head));

	/* flink_for_each_safe() and delete all test */
	flink_add_front(&node1.link, &head);
	flink_check_entries(&head, &node1, &node3, &node2);
	flink_for_each_safe(pos, prev, n, &head)
		flink_del_next(prev, &head);
	test_assert(flink_empty(&head));
}
TEST_DEFINE(TEST_UNIT, "flink", test_flink);

int main(int argc, char *argv[])
{
	test_init(argc, argv);

	test_run(NULL);

	return test_failures();
}
