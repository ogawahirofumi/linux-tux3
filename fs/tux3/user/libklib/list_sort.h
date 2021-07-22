#ifndef LIBKLIB_LIST_SORT_H
#define LIBKLIB_LIST_SORT_H

#include <libklib/types.h>

struct list_head;

typedef int __attribute__((nonnull(2,3))) (*list_cmp_func_t)(void *,
		const struct list_head *, const struct list_head *);

__attribute__((nonnull(2,3)))
void list_sort(void *priv, struct list_head *head, list_cmp_func_t cmp);
#endif /* !LIBKLIB_LIST_SORT_H */
