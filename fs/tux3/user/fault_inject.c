#ifdef FAULT_INJECTION
#include <stdio.h>
#include <execinfo.h>
#include <string.h>
#include <time.h>
#include <fnmatch.h>
#include <malloc.h>
#include <dlfcn.h>

#include "fault_inject.h"

#define fi_print(fmt, ...)		do {			\
	fprintf(stderr, "Fault Inject: " fmt, ##__VA_ARGS__);	\
} while (0)

unsigned fault_manager_seq = 0;

static struct fault_manager {
	struct list_head patterns;
	struct fault_info *last_fault;

	unsigned recursive;
	long random_seed;
	struct drand48_data random_buf;
} fault_manager = {
	.patterns = LIST_HEAD_INIT(fault_manager.patterns),
};

static struct fault_state *check_call_path(struct fault_info *info)
{
	struct fault_state state = {};
	int i;

	state.nr = backtrace(state.backtrace, MAX_CALL_DEPTH);
	if (state.nr == MAX_CALL_DEPTH)
		fi_print("Warning: MAX_CALL_DEPTH is smaller than require\n");

	for (i = 0; i < ARRAY_SIZE(info->state); i++) {
		/* End of array */
		if (info->state[i].backtrace[0] == NULL) {
			info->state[i] = state;
			return &info->state[i];
		}
		/* Found */
		if (info->state[i].nr == state.nr &&
		    !memcmp(info->state[i].backtrace, state.backtrace,
			    sizeof(info->state[i].backtrace[9]) * state.nr))
			return &info->state[i];
	}

	fi_print("Error: MAX_CALL_PATH is smaller than require\n");
	exit(1);
}

static int __fault_should_fail(struct fault_info *info)
{
	/* __fault_enable() changed config */
	if (FAULT_SEQ(info->seq_and_enable) != fault_manager_seq) {
		struct fault_pattern *pattern;

		info->seq_and_enable = fault_manager_seq;

		/* Make full identifier */
		snprintf(info->id, sizeof(info->id), "%s%s",
			 info->category, info->func);

		list_for_each_entry(pattern, &fault_manager.patterns, list) {
			int ret = fnmatch(pattern->pattern, info->id, 0);
			if (ret < 0 || ret == FNM_NOMATCH)
				continue;

			/* Disable this pattern */
			if (pattern->probability == 0)
				break;

			info->seq_and_enable |= FAULT_ENABLE;
			info->pattern = pattern;
			goto check;
		}

		/* This fault injection is disabling */
		info->seq_and_enable &= ~FAULT_ENABLE;
		return 0;
	}

check:;
	/* No config change from final __fault_enable() */

	struct fault_state *state;
	if (info->pattern->type & FAULT_PER_CALLPATH)
		state = check_call_path(info);
	else
		state = &info->state[0];

	if ((info->pattern->type & FAULT_ONE_SHOT) && state->hit > 0)
		return 0;

	long result;
	lrand48_r(&fault_manager.random_buf, &result);
	if ((result % 100) < info->pattern->probability) {
		state->hit++;

		/* Set latest hit */
		info->last_state = state;
		fault_manager.last_fault = info;

		if (info->pattern->type & FAULT_HAS_RET)
			info->ret = info->pattern->ret;

		fi_print("Injected fault at %s:%d return (%ld)\n",
			 info->func, info->line, info->ret);

		return 1;
	}

	return 0;
}

void fault_disable_injection(void)
{
	fault_manager.recursive++;
}

void fault_enable_injection(void)
{
	fault_manager.recursive--;
}

int fault_should_fail(struct fault_info *info)
{
	int ret = 0;

	/*
	 * If recursive, don't inject failure. This requires to use
	 * some functions in __fault_should_fail(). For example,
	 * libgcc may call malloc(), and goes our hook via backtrace().
	 */
	if (!fault_manager.recursive) {
		fault_disable_injection();
		ret = __fault_should_fail(info);
		fault_enable_injection();
	}
	return ret;
}

struct fault_info *fault_last_inject(void)
{
	return fault_manager.last_fault;
}

void fault_clear_last(void)
{
	fault_manager.last_fault = NULL;
}

void __fault_do_enable(struct fault_pattern *pattern)
{
	/* (Re)add to head */
	list_move(&pattern->list, &fault_manager.patterns);

	fault_manager_seq++;
	if (fault_manager_seq & FAULT_ENABLE)
		fault_manager_seq = 0;

	if (fault_manager.random_seed == 0) {
		fault_manager.random_seed = time(NULL);
		srand48_r(fault_manager.random_seed, &fault_manager.random_buf);
		fi_print("seed (%ld)\n",
			 fault_manager.random_seed);
	}
}

/*
 * Memory fault injection hooks for glibc
 */

#define HOOK_TO_GLIBC(err, rettype, name, proto, args...)	\
static rettype (*libc_##name) proto;				\
rettype name proto						\
{								\
	fault_return("memory:", err);				\
	if (libc_##name == NULL)				\
		libc_##name = dlsym(RTLD_NEXT, #name);		\
	return libc_##name(args);				\
}

HOOK_TO_GLIBC(NULL, void *, malloc, (size_t size), size);
HOOK_TO_GLIBC(NULL, void *, calloc, (size_t nmemb, size_t size), nmemb, size);
HOOK_TO_GLIBC(NULL, void *, realloc, (void *ptr, size_t size), ptr, size);
HOOK_TO_GLIBC(ENOMEM, int, posix_memalign,
	      (void **memptr, size_t alignment, size_t size),
	      memptr, alignment, size);
#endif /* FAULT_INJECTION */
