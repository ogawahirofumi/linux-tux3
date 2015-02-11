#ifndef LIBKLIB_SEQ_FILE_H
#define LIBKLIB_SEQ_FILE_H

#include <stdarg.h>
#include <stdbool.h>
#include <libklib/types.h>
#include <libklib/compiler.h>

struct seq_file {
	char *buf;
	size_t size;
	size_t count;
	void *private;
};

/*
 * seq_files have a buffer which can may overflow. When this happens a larger
 * buffer is reallocated and all the data will be printed again.
 * The overflow state is true when m->count == m->size.
 */
static inline bool seq_has_overflowed(struct seq_file *m)
{
	return m->count == m->size;
}

int seq_putc(struct seq_file *m, char c);
int seq_puts(struct seq_file *m, const char *s);

__printf(2, 3) int seq_printf(struct seq_file *, const char *, ...);
__printf(2, 0) int seq_vprintf(struct seq_file *, const char *, va_list args);

#endif /* !LIBKLIB_SEQ_FILE_H */
