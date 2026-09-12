/*
 * kernel/abilities/miniz/shim/string.h
 * -----------------------------------------------------------------------------
 * 内核 miniz 编译用的最小 <string.h>：转发到内核字符串库 <kernel/string.h>，
 * 并补充 miniz 可能用到的 memchr（实现在 kminiz.c）。
 */
#ifndef _KMINIZ_SHIM_STRING_H
#define _KMINIZ_SHIM_STRING_H

#include <kernel/string.h>

void *memchr(const void *s, int c, size_t n);

#endif /* _KMINIZ_SHIM_STRING_H */
