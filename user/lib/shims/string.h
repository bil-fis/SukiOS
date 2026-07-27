/*
 * user/lib/shims/string.h
 * -----------------------------------------------------------------------------
 * 最小 string.h 垫片，仅供独立 app（启用 -ffreestanding -nostdlib）编译
 * minimp3 时使用。minimp3 基础解码仅用到 memcpy/memset/memmove，这些符号由
 * user/lib/suki.c 提供强定义；这里仅给出原型声明（与 suki.h 重复声明无害）。
 */
#ifndef _SUKI_SHIM_STRING_H
#define _SUKI_SHIM_STRING_H

#include <stddef.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
void *memmove(void *d, const void *s, size_t n);
int   memcmp(const void *a, const void *b, size_t n);

#endif /* _SUKI_SHIM_STRING_H */
