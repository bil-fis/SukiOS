/*
 * include/mm/kstack.h
 * -----------------------------------------------------------------------------
 * P0-R5：带「未映射守卫页」的内核栈分配器。
 *
 * 背景：内核栈此前直接 kmalloc 于内核堆——堆内对象彼此紧邻，栈溢出会
 * 静默改写相邻分配块（页表、task_t、IPC 缓冲皆可能受害），只能靠栈底
 * canary 事后发现。本分配器把每条内核栈放进独立虚拟地址槽位，栈底下方
 * 保留一整页「不映射」的守卫页：溢出触碰守卫页立即 #PF（CR2 落在守卫
 * 区间），由 #PF/#DF 处理器给出「kernel stack overflow」定性诊断。
 *
 * 虚拟布局（KSTACK_AREA 与内核堆同属 PML4 槽 384，PDPT 级隔 2GB，
 * 保证在 kheap 首次映射后创建的所有地址空间自动共享本区页表）：
 *
 *   slot N: +0        +4KB                    +20KB
 *           | 守卫页  |  栈页 x4（RW, NX）     |
 *           ^未映射   ^kstack_base             ^kstack_top
 *
 * 物理页按页离散分配（无需连续）；释放时逐页归还 PMM 并解除映射
 * （vmm_unmap_page 对内核 VA 自动广播 TLB shootdown）。
 */
#ifndef _SUKI_MM_KSTACK_H
#define _SUKI_MM_KSTACK_H

#include <kernel/types.h>

/* 区域基址：KHEAP_BASE + 2GB —— 与堆同一 PML4 槽（内核 PML4[384] 在
 * kheap_init 时已建立并被所有地址空间共享），但 PDPT 项完全分离，
 * 堆上限 1GB 永远触不到本区。 */
#define KSTACK_AREA_BASE   0xFFFFC00080000000UL

#define KSTACK_GUARD_PAGES 1
#define KSTACK_PAGES       4                       /* 16KB 栈体 */
#define KSTACK_SLOT_PAGES  (KSTACK_GUARD_PAGES + KSTACK_PAGES)
#define KSTACK_SLOT_BYTES  (KSTACK_SLOT_PAGES * PAGE_SIZE)
#define KSTACK_BYTES       (KSTACK_PAGES * PAGE_SIZE)
#define KSTACK_MAX_SLOTS   512                     /* 512 条栈 = 10MB VA 窗口 */
#define KSTACK_AREA_END    (KSTACK_AREA_BASE + \
                            (uint64_t)KSTACK_MAX_SLOTS * KSTACK_SLOT_BYTES)

/* 分配一条 16KB 内核栈，返回栈底 VA（低端，向上可用 KSTACK_BYTES）。
 * 失败返回 0。栈页 RW+NX；栈底正下方一页保证未映射。 */
uint64_t kstack_alloc(void);

/* 释放 kstack_alloc 返回的栈（传入当初的栈底 VA）。 */
void kstack_free(uint64_t kstack_base);

/* addr 是否落在某条「已分配栈」的守卫页内（#PF/#DF 处理器用来把
 * 神秘缺页定性为内核栈溢出）。 */
bool kstack_guard_hit(uint64_t addr);

#endif /* _SUKI_MM_KSTACK_H */
