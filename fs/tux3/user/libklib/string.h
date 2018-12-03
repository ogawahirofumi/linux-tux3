/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LIBKLIB_STRING_H
#define LIBKLIB_STRING_H

#include <sys/types.h>
#include <libklib/compiler.h>

size_t strlcpy(char *, const char *, size_t);
ssize_t strscpy(char *, const char *, size_t);
extern size_t strlcat(char *, const char *, size_t);

#endif /* LIBKLIB_STRING_H */
