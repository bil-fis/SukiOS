/*
 * kernel/mm/pmm.c
 * -----------------------------------------------------------------------------
 * 位图式物理内存管理器。
 *
 * 初始化流程：
 *   1. 由 Multiboot2 内存映射得出最高物理地址 -> 计算总页数。
 *   2. 在内核末尾之后放置位图与引用计数数组（物理连续）。
 *   3. 先将全部页标记为"已用"，再依据 mmap 的可用区域(type=1)标记为"空闲"。
 *   4. 重新占用：低端 1MB、内核映像、PMM 元数据区。
 *
 * 调用关系：kmain() -> pmm_init(); vmm/kmalloc -> pmm_alloc_page()。
 */
#include <mm/pmm.h>
#include <kernel/string.h>
#include <kernel/console.h>

extern char __kernel_phys_end[];   /* 链接脚本：内核物理结束地址 */

static uint8_t  *g_bitmap;          /* 虚拟指针：每 bit 一页 */
static uint32_t *g_refcount;        /* 每页引用计数 */
static uint64_t  g_total_pages;
static uint64_t  g_used_pages;
static uint64_t  g_meta_end_phys;   /* PMM 元数据物理结束（含内核） */

#define BIT_IDX(pg)   ((pg) >> 3)
#define BIT_OFF(pg)   ((pg) & 7)

static inline void bm_set(uint64_t pg)   { g_bitmap[BIT_IDX(pg)] |=  (1u << BIT_OFF(pg)); }
static inline void bm_clear(uint64_t pg) { g_bitmap[BIT_IDX(pg)] &= ~(1u << BIT_OFF(pg)); }
static inline bool bm_test(uint64_t pg)  { return g_bitmap[BIT_IDX(pg)] & (1u << BIT_OFF(pg)); }

static uint64_t align_up(uint64_t x, uint64_t a) { return (x + a - 1) & ~(a - 1); }

void pmm_init(const boot_info_t *bi)
{
    uint64_t highest = bi->mem_highest;
    if (highest == 0) {
        highest = 0x8000000;   /* 回退 128MB */
    }
    g_total_pages = highest / PAGE_SIZE;

    uint64_t bitmap_bytes   = align_up(g_total_pages / 8, 8);
    uint64_t refcount_bytes = g_total_pages * sizeof(uint32_t);

    /* 元数据放在内核末尾之后（物理连续，均 < 4GB，可经 PHYS_TO_VIRT 访问） */
    uint64_t meta_phys = align_up((uint64_t)__kernel_phys_end, PAGE_SIZE);
    g_bitmap   = (uint8_t  *)PHYS_TO_VIRT(meta_phys);
    g_refcount = (uint32_t *)PHYS_TO_VIRT(meta_phys + bitmap_bytes);
    g_meta_end_phys = align_up(meta_phys + bitmap_bytes + refcount_bytes, PAGE_SIZE);

    /* 全部标记为已用 */
    memset(g_bitmap, 0xFF, bitmap_bytes);
    memset(g_refcount, 0, refcount_bytes);
    g_used_pages = g_total_pages;

    /* 依据 mmap 释放可用 RAM */
    if (bi->mmap) {
        const struct mb2_tag_mmap *mm = bi->mmap;
        uint32_t esz = mm->entry_size;
        const uint8_t *e  = (const uint8_t *)mm->entries;
        const uint8_t *me = (const uint8_t *)mm + mm->size;
        while (e + esz <= me) {
            const struct mb2_mmap_entry *ent = (const struct mb2_mmap_entry *)e;
            if (ent->type == 1) {   /* 可用 */
                uint64_t start = align_up(ent->addr, PAGE_SIZE);
                uint64_t end   = (ent->addr + ent->len) & ~(PAGE_SIZE - 1);
                for (uint64_t a = start; a < end; a += PAGE_SIZE) {
                    uint64_t pg = a / PAGE_SIZE;
                    if (pg < g_total_pages && bm_test(pg)) {
                        bm_clear(pg);
                        g_used_pages--;
                    }
                }
            }
            e += esz;
        }
    }

    /* 重新占用低端 1MB + 内核 + PMM 元数据区 [0, g_meta_end_phys) */
    for (uint64_t a = 0; a < g_meta_end_phys; a += PAGE_SIZE) {
        uint64_t pg = a / PAGE_SIZE;
        if (pg < g_total_pages && !bm_test(pg)) {
            bm_set(pg);
            g_used_pages++;
        }
    }

    kprintf("[pmm] total=%u MiB, pages=%lu, used=%lu, free=%lu, meta_end=%p\n",
            (unsigned)(g_total_pages * PAGE_SIZE / (1024 * 1024)),
            (unsigned long)g_total_pages,
            (unsigned long)g_used_pages,
            (unsigned long)(g_total_pages - g_used_pages),
            (void *)g_meta_end_phys);
}

void *pmm_alloc_page(void)
{
    for (uint64_t pg = g_meta_end_phys / PAGE_SIZE; pg < g_total_pages; pg++) {
        if (!bm_test(pg)) {
            bm_set(pg);
            g_used_pages++;
            g_refcount[pg] = 1;
            void *phys = (void *)(pg * PAGE_SIZE);
            /* 清零新页（经高半区映射访问） */
            memset(PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
            return phys;
        }
    }
    return NULL;   /* 内存耗尽 */
}

void pmm_free_page(void *phys_addr)
{
    uint64_t pg = (uint64_t)phys_addr / PAGE_SIZE;
    if (pg >= g_total_pages || !bm_test(pg)) {
        return;
    }
    bm_clear(pg);
    g_refcount[pg] = 0;
    g_used_pages--;
}

void pmm_incref(void *phys_addr)
{
    uint64_t pg = (uint64_t)phys_addr / PAGE_SIZE;
    if (pg < g_total_pages) {
        g_refcount[pg]++;
    }
}

uint64_t pmm_decref(void *phys_addr)
{
    uint64_t pg = (uint64_t)phys_addr / PAGE_SIZE;
    if (pg >= g_total_pages || g_refcount[pg] == 0) {
        return 0;
    }
    g_refcount[pg]--;
    uint64_t rem = g_refcount[pg];
    if (rem == 0) {
        pmm_free_page(phys_addr);
    }
    return rem;
}

uint64_t pmm_total_pages(void) { return g_total_pages; }
uint64_t pmm_used_pages(void)  { return g_used_pages; }
uint64_t pmm_free_pages(void)  { return g_total_pages - g_used_pages; }
