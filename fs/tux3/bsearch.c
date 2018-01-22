#ifndef BSEARCH_NAME
#error "BSEARCH_NAME is undefined"
#endif
#ifndef BSEARCH_COMPARE
#error "BSEARCH_COMPARE is undefined"
#endif
#ifndef BSEARCH_ENT_SIZE
#error "BSEARCH_ENT_SIZE is undefined"
#endif

/*
 * Search idx <= key.
 *
 * return value:
 * >= 0 - closest idx
 *   -1 - all idx are larger than key, or istart > count - 1
 */
static int BSEARCH_NAME(void *data, int count, const void *key,
			const int istart)
{
	int imin = istart, imax = count - 1;

	while (imax - imin > 0) {
		int idx = imin + (imax - imin) / 2;
		void *p = data + (idx * BSEARCH_ENT_SIZE);
		int ret = BSEARCH_COMPARE(p, key);
		if (ret < 0)
			imin = idx + 1;
		else if (ret > 0)
			imax = idx - 1;
		else
			return idx;
	}

	if (imin == imax) {
		/* 1 entry is remaining to compare */
		void *p = data + (imin * BSEARCH_ENT_SIZE);
		if (BSEARCH_COMPARE(p, key) > 0)
			imax--;
	} else /* imin > imax */ {
		/* p > key on latest compare when 2 entries are remaining,
		 * or istart > bd_count-1 */
	}
	if (imax < istart)
		imax = -1;
	return imax;
}

#undef BSEARCH_NAME
#undef BSEARCH_COMPARE
#undef BSEARCH_ENT_SIZE
