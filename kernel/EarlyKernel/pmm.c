/* =============================================================================
 * SukiOS - 物理内存管理器（PMM）
 *
 * 使用位图管理物理页帧，bit=0 表示空闲，bit=1 表示已使用。
 * 参考：OSDev Page Frame Allocation, Writing A Page Frame Allocator
 *
 * 初始化流程：
 *   1. 将所有页标记为已使用
 *   2. 遍历 Multiboot2 内存映射，将可用区域标记为空闲
 *   3. 将内核占用的物理内存标记为已使用
 * ============================================================================= */

#include "kernel/pmm.h"
#include "kernel/tty.h"

/* 位图：每个 bit 对应一个物理页帧 */
static uint8_t pmm_bitmap[PMM_MAX_PAGES / 8];

/* 内存统计 */
static uint64_t total_pages;
static uint64_t used_pages;

/* 内核结束物理地址（由链接脚本提供）
 *
 * 链接脚本中 PROVIDE(__kernel_phys_end = LOADADDR(.bss) + SIZEOF(.bss))
 * 创建的符号 __kernel_phys_end 的【地址】等于物理结束地址。
 * 因此用 extern char[] 声明，取符号地址 (&) 即为物理结束地址。 */
extern char __kernel_phys_end[];

/* 简易 memset */
static void mem_set(void *dst, int val, size_t n)
{
    uint8_t *p = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t)val;
}

static inline void bitmap_set(uint64_t page)
{
    if (page < PMM_MAX_PAGES)
        pmm_bitmap[page / 8] |= (1 << (page % 8));
}

static inline void bitmap_clear(uint64_t page)
{
    if (page < PMM_MAX_PAGES)
        pmm_bitmap[page / 8] &= ~(1 << (page % 8));
}

static inline int bitmap_test(uint64_t page)
{
    if (page >= PMM_MAX_PAGES) return 1; /* 超出范围视为已使用 */
    return (pmm_bitmap[page / 8] >> (page % 8)) & 1;
}

void pmm_init(struct multiboot2_mmap_entry *entries, uint32_t count)
{
    /* 1. 将所有页标记为已使用 */
    mem_set(pmm_bitmap, 0xFF, sizeof(pmm_bitmap));
    total_pages = 0;
    used_pages = 0;

    /* 2. 遍历 Multiboot2 内存映射，将可用区域标记为空闲 */
    for (uint32_t i = 0; i < count; i++) {
        if (entries[i].type != MULTIBOOT_MEMORY_AVAILABLE)
            continue;

        uint64_t base = entries[i].addr;
        uint64_t end  = base + entries[i].len;

        /* 对齐到页边界 */
        uint64_t start_page = (base + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t end_page   = end / PAGE_SIZE;

        for (uint64_t p = start_page; p < end_page && p < PMM_MAX_PAGES; p++) {
            bitmap_clear(p);
            total_pages++;
        }
    }

    /* 3. 将内核占用的物理内存标记为已使用
     * 包括页表（0x70000-0x76FFF）、bootstrap（0x100000-0x10FFFF）、
     * 以及内核代码/数据/BSS（0x110000 - __kernel_phys_end） */
    uint64_t kernel_end_phys = (uint64_t)(uintptr_t)__kernel_phys_end;
    uint64_t kernel_end_page = (kernel_end_phys + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t p = 0; p < kernel_end_page && p < PMM_MAX_PAGES; p++) {
        if (!bitmap_test(p)) {
            bitmap_set(p);
            used_pages++;
            total_pages--;
        }
    }
}

uint64_t pmm_alloc_page(void)
{
    for (uint64_t i = 0; i < PMM_MAX_PAGES; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_pages++;
            total_pages--;
            return i * PAGE_SIZE;
        }
    }
    return 0; /* 无空闲页 */
}

void pmm_free_page(uint64_t phys_addr)
{
    uint64_t page = phys_addr / PAGE_SIZE;
    if (page < PMM_MAX_PAGES && bitmap_test(page)) {
        bitmap_clear(page);
        used_pages--;
        total_pages++;
    }
}

uint64_t pmm_get_total_pages(void)
{
    return total_pages;
}

uint64_t pmm_get_used_pages(void)
{
    return used_pages;
}

uint64_t pmm_get_free_pages(void)
{
    return total_pages;
}
