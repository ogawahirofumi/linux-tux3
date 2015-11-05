#ifndef TUX3_ALIGNCHECK_H
#define TUX3_ALIGNCHECK_H

#if defined(ALIGN_CHECK) && (defined(__x86_64__) || defined(__i386__))
void enable_alignment_check(void);
void disable_alignment_check(void);
void init_alignment_check(void);
#else
#define enable_alignment_check()	do {} while (0)
#define disable_alignment_check()	do {} while (0)
#define init_alignment_check()		do {} while (0)
#endif

#endif /* !TUX3_ALIGNCHECK_H */
