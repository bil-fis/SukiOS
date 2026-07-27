/*
 * include/mm/pmm.h
 * -----------------------------------------------------------------------------
 * 物理内存管理器 (PMM)。位图式分配（每 bit 代表 1 个 4KB 页），并维护
 * 引用计数数组用于 IPC OOL 零拷贝共享页（手册 4.2 / 7.1）。
 *
 * 约定：pmm_alloc_page() 返回 4KB 对齐的**物理地址**；访问其内容须经
 *       PHYS_TO_VIRT()（内核高半区已映射全部低于 4GB 的物理内存）。
 */
#ifndef _SUKI_MM_PMM_H
#define _SUKI_MM_PMM_H

#include <kernel/types.h>
#include <kernel/multiboot2.h>

/* 手册 4.2 定义的页帧描述符（保留 ABI；本实现以位图为核心） */
typedef struct page_frame {
    uint64_t base_addr;
    uint64_t ref_count;
    struct page_frame *next;
} page_frame_t;

void   pmm_init(const boot_info_t *bi);
void  *pmm_alloc_page(void);              /* 返回物理地址，失败返回 NULL */
void   pmm_free_page(void *phys_addr);

/* 引用计数（OOL 共享页） */
void     pmm_incref(void *phys_addr);
uint64_t pmm_decref(void *phys_addr);     /* 减 1，返回剩余计数；到 0 自动释放。
                                           * 饱和页(0xFFFFFFFF)粘滞：永不递减/释放 */
uint64_t pmm_refcount(void *phys_addr);   /* 审计/自检用：读取当前引用计数 */

/* M2 审计自检：OOL 共享压测 + 饱和粘滞 + 共享页守卫（boot 时调用） */
void pmm_selftest(void);

/* 统计 */
uint64_t pmm_total_pages(void);
uint64_t pmm_used_pages(void);
uint64_t pmm_free_pages(void);

#endif /* _SUKI_MM_PMM_H */
