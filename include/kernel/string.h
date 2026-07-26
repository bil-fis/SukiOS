/*
 * include/kernel/string.h
 * -----------------------------------------------------------------------------
 * 内核自由环境字符串/内存库。提供 GCC 在 -O2 下可能隐式生成调用的
 * memset/memcpy/memmove/memcmp，以及常用字符串工具。
 */
#ifndef _SUKI_KERNEL_STRING_H
#define _SUKI_KERNEL_STRING_H

#include <kernel/types.h>

void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strchr(const char *s, int c);

#endif /* _SUKI_KERNEL_STRING_H */
