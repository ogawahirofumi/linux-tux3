#ifndef LIBKLIB_BUG_H
#define LIBKLIB_BUG_H

#include <stdio.h>

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

/*
 * WARN(), WARN_ON(), WARN_ON_ONCE, and so on can be used to report
 * significant issues that need prompt attention if they should ever
 * appear at runtime.  Use the versions with printk format strings
 * to provide better diagnostics.
 */
#define __WARN()		do {				\
	fprintf(stderr, "WARNING: at %s:%d/%s()\n",		\
		__FILE__, __LINE__, __func__);			\
} while (0)
#define __WARN_printf(arg...)	do { __WARN(); fprintf(stderr, arg); } while (0)

#ifndef WARN_ON
#define WARN_ON(condition) ({						\
	int __ret_warn_on = !!(condition);				\
	if (unlikely(__ret_warn_on))					\
		__WARN();						\
	unlikely(__ret_warn_on);					\
})
#endif

#ifndef WARN
#define WARN(condition, format...) ({					\
	int __ret_warn_on = !!(condition);				\
	if (unlikely(__ret_warn_on))					\
		__WARN_printf(format);					\
	unlikely(__ret_warn_on);					\
})
#endif

#define WARN_ON_ONCE(condition)	({				\
	static bool __warned;					\
	int __ret_warn_once = !!(condition);			\
								\
	if (unlikely(__ret_warn_once))				\
		if (WARN_ON(!__warned)) 			\
			__warned = true;			\
	unlikely(__ret_warn_once);				\
})

#define WARN_ONCE(condition, format...)	({			\
	static bool __warned;					\
	int __ret_warn_once = !!(condition);			\
								\
	if (unlikely(__ret_warn_once))				\
		if (WARN(!__warned, format)) 			\
			__warned = true;			\
	unlikely(__ret_warn_once);				\
})

#endif /* !LIBKLIB_BUG_H */
