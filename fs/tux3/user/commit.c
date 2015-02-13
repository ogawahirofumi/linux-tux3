#include "tux3user.h"

#include "../commit.c"

/* Force flush (even if no dirty) with unify */
int force_unify(struct sb *sb)
{
	return __sync_current_delta(sb, FORCE_UNIFY);
}

/* Force flush (even if no dirty) without unify */
int force_delta(struct sb *sb)
{
	return __sync_current_delta(sb, FORCE_DELTA);
}

/* Normal flush */
int sync_super(struct sb *sb)
{
	return __sync_current_delta(sb, FLUSH_NORMAL);
}
