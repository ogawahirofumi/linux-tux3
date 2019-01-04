/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LIBKLIB_STRING_H
#define LIBKLIB_STRING_H

#include <sys/types.h>
#include <string.h>
#include <libklib/compiler.h>

size_t strlcpy(char *, const char *, size_t);
ssize_t strscpy(char *, const char *, size_t);
extern size_t strlcat(char *, const char *, size_t);

extern char *kstrdup(const char *s, gfp_t gfp) __malloc;
extern char *kstrndup(const char *s, size_t len, gfp_t gfp);
extern void *kmemdup(const void *src, size_t len, gfp_t gfp);
extern char *kmemdup_nul(const char *s, size_t len, gfp_t gfp);

#endif /* LIBKLIB_STRING_H */
