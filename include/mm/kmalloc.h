/*
 * include/mm/kmalloc.h
 * -----------------------------------------------------------------------------
 * 内核堆分配器。基于一个按需映射增长的高半区虚拟堆区，首次适配 + 合并。
 */
#ifndef _SUKI_MM_KMALLOC_H
#define _SUKI_MM_KMALLOC_H

#include <kernel/types.h>

/* 内核堆虚拟基址（高半区，位于内核 PML4 项 384，随内核映射共享） */
#define KHEAP_BASE   0xFFFFC00000000000UL
#define KHEAP_MAX    (KHEAP_BASE + 0x40000000UL)   /* 上限 1GB */

void  kheap_init(void);
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void  kfree(void *ptr);

#endif /* _SUKI_MM_KMALLOC_H */
