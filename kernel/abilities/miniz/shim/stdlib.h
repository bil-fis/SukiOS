/*
 * kernel/abilities/miniz/shim/stdlib.h
 * -----------------------------------------------------------------------------
 * 内核 miniz 编译用的最小 <stdlib.h>：仅声明 miniz 的分配接口，
 * 实现在 kernel/abilities/miniz/kminiz.c（映射到内核堆 kmalloc/kzalloc/krealloc/kfree）。
 */
#ifndef _KMINIZ_SHIM_STDLIB_H
#define _KMINIZ_SHIM_STDLIB_H

#include <stddef.h>

void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void  free(void *ptr);

#endif /* _KMINIZ_SHIM_STDLIB_H */
