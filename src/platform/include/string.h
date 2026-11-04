/* string.h - the four calls we have (libc_min.c / md_libc.c): the
 * toolchain is built without a C library. */
#ifndef SEGATUBE_STRING_H
#define SEGATUBE_STRING_H
#include <stddef.h>
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
#endif
