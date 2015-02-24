#ifndef LIBKLIB_BUG_H
#define LIBKLIB_BUG_H

/*
 * Don't use BUG() or BUG_ON() unless there's really no way out; one
 * example might be detecting data structure corruption in the middle
 * of an operation that can't be backed out of.  If the (sub)system
 * can somehow continue operating, perhaps with reduced functionality,
 * it's probably not BUG-worthy.
 *
 * If you're tempted to BUG(), think again:  is completely giving up
 * really the *only* solution?  There are usually better options, where
 * users don't need to reboot ASAP and can mostly shut down cleanly.
 */
#define BUG() do {						\
	fprintf(stderr, "BUG: failure at %s:%d/%s()!\n",	\
		__FILE__, __LINE__, __func__);			\
	{ int *__p = NULL; *__p = 1; }				\
} while (0)

#define BUG_ON(condition) do { if (unlikely(condition)) BUG(); } while (0)

#endif /* !LIBKLIB_BUG_H */
