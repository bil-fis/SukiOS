/* =============================================================================
 * SukiOS - 内核堆分配器
 *
 * 使用链表式空闲块管理器（first-fit 算法）。
 * 每个块头部 16 字节（size + next），最小分配 32 字节。
 *
 * 参考：OSDev Kernel Memory Management, K&R malloc
 * ============================================================================= */

#ifndef SUKIOS_HEAP_H
#define SUKIOS_HEAP_H

#include <stdint.h>
#include <stddef.h>

/**
 * heap_init - 初始化内核堆
 * @start:  堆起始虚拟地址
 * @size:   堆大小（字节）
 */
void heap_init(uint64_t start, size_t size);

/**
 * kmalloc - 从内核堆分配内存
 * @size:   请求的字节数
 * @return: 分配的内存指针，NULL 表示失败
 */
void *kmalloc(size_t size);

/**
 * kfree - 释放内核堆内存
 * @ptr:    之前由 kmalloc 返回的指针
 */
void kfree(void *ptr);

/**
 * krealloc - 重新分配内核堆内存
 * @ptr:    之前的指针（NULL 等同于 kmalloc）
 * @size:   新的大小（0 等同于 kfree）
 * @return: 新的内存指针
 */
void *krealloc(void *ptr, size_t size);

/**
 * kcalloc - 分配并清零内存
 * @nmemb:  元素个数
 * @size:   每个元素的大小
 * @return: 分配的内存指针
 */
void *kcalloc(size_t nmemb, size_t size);

/**
 * heap_get_stats - 获取堆使用统计
 * @total:  总堆大小
 * @used:   已使用大小
 * @free:   空闲大小
 */
void heap_get_stats(size_t *total, size_t *used, size_t *free);

#endif /* SUKIOS_HEAP_H */
