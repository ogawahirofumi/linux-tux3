#ifndef LIBKLIB_SEQ_FILE_H
#define LIBKLIB_SEQ_FILE_H

#include <stdarg.h>
#include <libklib/types.h>
#include <libklib/compiler.h>

struct seq_file {
	char *buf;
	size_t size;
	size_t count;
	void *private;
};

/**
 * seq_has_overflowed - check if the buffer has overflowed
 * @m: the seq_file handle
 *
 * seq_files have a buffer which may overflow. When this happens a larger
 * buffer is reallocated and all the data will be printed again.
 * The overflow state is true when m->count == m->size.
 *
 * Returns true if the buffer received more than it can hold.
 */
static inline bool seq_has_overflowed(struct seq_file *m)
{
	return m->count == m->size;
}

__printf(2, 0)
void seq_vprintf(struct seq_file *m, const char *fmt, va_list args);
__printf(2, 3)
void seq_printf(struct seq_file *m, const char *fmt, ...);
void seq_putc(struct seq_file *m, char c);
void seq_puts(struct seq_file *m, const char *s);

#endif /* !LIBKLIB_SEQ_FILE_H */
