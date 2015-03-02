#ifndef TUX3_FAULT_INJECT_H
#define TUX3_FAULT_INJECT_H

#ifdef FAULT_INJECTION
#include <libklib/libklib.h>

/*
 * Fault injection interfaces.
 *
 * fault_disable_injection():
 * fault_enable_injection():
 * Enable/disable fault injection temporary.
 *
 * fault_return(category_str, default_ret):
 * @category_str: identifier to enable/disable this fault injection.
 * full identifier become @category_str + __func__.
 * @default_ret: default return value when injecting fault.
 *
 * fault_enable_ret(str, type, probability, ret):
 * fault_enable(str, type, probability):
 * fault_disable(str):
 * @str: pattern to match full identifier by fnmatch().
 * @type: injection type.
 * @probability: percentage to inject fault.
 * @ret: return value to be used at fault_return().
 *
 * fault_last_inject()
 * fault_clear_last()
 * Get/clear last injected failure.
 */

/* One shot fault inject */
#define FAULT_ONE_SHOT		(1 << 0)
/* Check the call path, and treat each paths are different */
#define FAULT_PER_CALLPATH	(1 << 2)

/* Internal flag to change return value */
#define FAULT_HAS_RET		(1 << 31)

struct fault_pattern {
	struct list_head list;
	const char *pattern;
	unsigned type;
	int probability;
	unsigned long ret;
};

extern unsigned fault_manager_seq;
void __fault_do_enable(struct fault_pattern *pattern);

#define fault_do_enable(str, t, p, r) do {			\
	static struct fault_pattern pattern = {			\
		.list		= LIST_HEAD_INIT(pattern.list),	\
		.pattern	= str,				\
		.type		= t,				\
		.probability	= p,				\
	};							\
	pattern.ret = (unsigned long)(r);			\
	__fault_do_enable(&pattern);				\
} while (0)

#define fault_disable(str) do {					\
	fault_do_enable(str, 0, 0, 0);				\
} while (0)

#define fault_enable_ret(str, t, p, r) do {			\
	fault_do_enable(str, t | FAULT_HAS_RET, p, r);		\
} while (0)

#define fault_enable(str, t, p) do {				\
	fault_do_enable(str, t, p, 0);				\
} while (0)

/* FIXME: This is using static allocation to make simple memory management */
#define MAX_CALL_PATH		256
#define MAX_CALL_DEPTH		32

/* Per caller state */
struct fault_state {
	int nr;
	void *backtrace[MAX_CALL_DEPTH];
	unsigned hit;
};

struct fault_info {
	char id[128];
	const char *category;
	const char *func;
	const int line;

#define FAULT_ENABLE		(1U << 31)
#define FAULT_SEQ(x)		((x) & ~FAULT_ENABLE)
	unsigned seq_and_enable;
	struct fault_pattern *pattern;
	unsigned long ret;

	struct fault_state *last_state;
	struct fault_state state[MAX_CALL_PATH];
};

void fault_disable_injection(void);
void fault_enable_injection(void);
int fault_should_fail(struct fault_info *info);
struct fault_info *fault_last_inject(void);
void fault_clear_last(void);

#define fault_return(category_str, default_ret) do {			\
	static struct fault_info __fault_info = {			\
		.category	= category_str,				\
		.func		= __func__,				\
		.line		= __LINE__,				\
	};								\
	if (__fault_info.seq_and_enable != fault_manager_seq) {		\
		__fault_info.ret = (unsigned long)(default_ret);	\
		if (fault_should_fail(&__fault_info))			\
			return (typeof(default_ret))__fault_info.ret;	\
	}								\
} while (0)

#else /* !FAULT_INJECTION */
#define fault_disable(str)		do {} while (0)
#define fault_enable(str, t, p)		do {} while (0)
#define fault_enable_ret(str, t, p, r)	do {} while (0)
#define fault_last_inject()		NULL
#define fault_clear_last()		do {} while (0)
#define fault_disable_injection()	do {} while (0)
#define fault_enable_injection()	do {} while (0)
#define fault_return(id_str, ret)	do {} while (0)
#endif /* !FAULT_INJECTION */

#endif /* !TUX3_FAULT_INJECT_H */
