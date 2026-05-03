#ifndef SUKIOS_HEAP_H
#define SUKIOS_HEAP_H

#include <stdint.h>
#include <stddef.h>

/* 最大单次分配（256 MB），防整数溢出 */
#define HEAP_MAX_ALLOC (256UL * 1024 * 1024)

/* 初始化 */
void heap_init(uint64_t start, size_t size);

/* 核心分配 */
void *kmalloc(size_t size);
void  kfree(void *ptr);
void *krealloc(void *ptr, size_t size);
void *kcalloc(size_t nmemb, size_t size);

/* 统计 */
void heap_get_stats(size_t *total, size_t *used, size_t *free);

/* 调试 */
void heap_dump(void);
int  heap_check(void);

#endif