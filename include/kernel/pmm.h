/* =============================================================================
 * SukiOS - 物理内存管理器（PMM）
 *
 * 使用位图管理物理页帧：
 *   bit=0 表示页帧空闲，bit=1 表示页帧已使用
 *
 * 参考：OSDev Page Frame Allocation, Writing A Page Frame Allocator
 * ============================================================================= */

#ifndef SUKIOS_PMM_H
#define SUKIOS_PMM_H

#include <stdint.h>
#include "multiboot2.h"

#define PAGE_SIZE 4096

/* 最大支持的物理内存（4 GB = 1048576 页） */
#define PMM_MAX_PAGES (4ULL * 1024 * 1024 * 1024 / PAGE_SIZE)

void pmm_init(struct multiboot2_mmap_entry *entries, uint32_t count);
uint64_t pmm_alloc_page(void);
void pmm_free_page(uint64_t phys_addr);
uint64_t pmm_get_total_pages(void);
uint64_t pmm_get_used_pages(void);
uint64_t pmm_get_free_pages(void);

#endif /* SUKIOS_PMM_H */
