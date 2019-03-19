/*
 * lib/parser.c - simple seq_file emulation
 *
 * This source code is licensed under the GNU General Public License,
 * Version 2.  See the file COPYING for more details.
 */

#include <stdio.h>
#include <string.h>
#include <libklib/seq_file.h>

static void seq_set_overflow(struct seq_file *m)
{
	m->count = m->size;
}

void seq_vprintf(struct seq_file *m, const char *f, va_list args)
{
	int len;

	if (m->count < m->size) {
		len = vsnprintf(m->buf + m->count, m->size - m->count, f, args);
		if (m->count + len < m->size) {
			m->count += len;
			return;
		}
	}
	seq_set_overflow(m);
}

void seq_printf(struct seq_file *m, const char *f, ...)
{
	va_list args;

	va_start(args, f);
	seq_vprintf(m, f, args);
	va_end(args);
}

void seq_putc(struct seq_file *m, char c)
{
	if (m->count >= m->size)
		return;

	m->buf[m->count++] = c;
}

void seq_puts(struct seq_file *m, const char *s)
{
	int len = strlen(s);

	if (m->count + len >= m->size) {
		seq_set_overflow(m);
		return;
	}
	memcpy(m->buf + m->count, s, len);
	m->count += len;
}
