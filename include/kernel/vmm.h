/* =============================================================================
 * SukiOS - 虚拟内存管理器（VMM）
 *
 * 提供页表操作接口：映射、解映射、属性变更。
 * 管理内核高半区虚拟地址空间。
 *
 * 参考：OSDev Paging, Intel SDM Vol.3 Chapter 4 (Paging)
 *
 * 当前虚拟地址空间布局：
 *   0xFFFFFFFF80000000 - 内核代码/数据（由 boot.asm 建立）
 *   0xFFFFFFFFC0000000 - MMIO 映射区域（APIC, IOAPIC 等）
 *   0xFFFFFFFF80000000 + 512MB = 0xFFFFFFFFA0000000 - 内核堆区域
 * ============================================================================= */

#ifndef SUKIOS_VMM_H
#define SUKIOS_VMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---- 页大小 ---- */
#define VMM_PAGE_SIZE   4096

/* ---- PTE/PDE 标志位 ---- */
#define VMM_PRESENT     (1ULL << 0)
#define VMM_WRITE       (1ULL << 1)
#define VMM_USER        (1ULL << 2)
#define VMM_PWT         (1ULL << 3)   /* Write-Through */
#define VMM_PCD         (1ULL << 4)   /* Cache Disable */
#define VMM_ACCESSED    (1ULL << 5)
#define VMM_DIRTY       (1ULL << 6)
#define VMM_PS          (1ULL << 7)   /* Page Size (2MB / 1GB) */
#define VMM_GLOBAL      (1ULL << 8)
#define VMM_XD          (1ULL << 63)  /* Execute Disable (NX) */

/* ---- 内核常用权限组合 ---- */
#define VMM_KERN_RW     (VMM_PRESENT | VMM_WRITE)              /* 内核读写 */
#define VMM_KERN_RO     (VMM_PRESENT)                          /* 内核只读 */
#define VMM_KERN_RW_CD  (VMM_PRESENT | VMM_WRITE | VMM_PCD | VMM_PWT)  /* 内核读写，Uncacheable（MMIO） */
#define VMM_USER_RW     (VMM_PRESENT | VMM_WRITE | VMM_USER)   /* 用户读写 */
#define VMM_USER_RO     (VMM_PRESENT | VMM_USER)               /* 用户只读 */

/* ---- MMIO 专用映射区域 ---- */
/* 0xFFFFFFFFC0000000 起，最多映射 1GB 的 MMIO 空间 */
#define VMM_MMIO_BASE   0xFFFFFFFFC0000000ULL
#define VMM_MMIO_SIZE   (512ULL * 1024 * 1024)  /* 512MB 预留 */
#define VMM_MMIO_PAGES  (VMM_MMIO_SIZE / VMM_PAGE_SIZE)

/* ---- 内核堆区域 ---- */
/* 紧跟在内核代码段之后 */
#define VMM_HEAP_BASE   0xFFFFFFFFA0000000ULL
#define VMM_HEAP_SIZE   (512ULL * 1024 * 1024)  /* 512MB 预留 */

/* ---- 虚拟地址 -> 页表索引 ---- */
static inline uint64_t vmm_pml4_index(uint64_t vaddr)
{
    return (vaddr >> 39) & 0x1FF;
}

static inline uint64_t vmm_pdpte_index(uint64_t vaddr)
{
    return (vaddr >> 30) & 0x1FF;
}

static inline uint64_t vmm_pd_index(uint64_t vaddr)
{
    return (vaddr >> 21) & 0x1FF;
}

static inline uint64_t vmm_pt_index(uint64_t vaddr)
{
    return (vaddr >> 12) & 0x1FF;
}

static inline uint64_t vmm_page_offset(uint64_t vaddr)
{
    return vaddr & 0xFFF;
}

/* ---- 页表结构体类型 ---- */
typedef uint64_t pml4e_t;
typedef uint64_t pdpte_t;
typedef uint64_t pde_t;
typedef uint64_t pte_t;

/* ---- 核心接口 ---- */

/**
 * vmm_init - 初始化虚拟内存管理器
 *
 * 从 boot.asm 的临时页表迁移到正式的内核页表：
 *   1. 分配新的 PML4
 *   2. 重新映射内核高半区（使用 2MB 大页覆盖 512MB）
 *   3. 保留低地址的完整身份映射（0-4GB）
 *   4. 激活新页表
 */
void vmm_init(void);

/**
 * vmm_map_page - 将一个物理页映射到虚拟地址
 * @phys:   物理页地址（必须页对齐）
 * @virt:   虚拟地址（必须页对齐）
 * @flags:  页表项标志（VMM_PRESENT | VMM_WRITE 等）
 * @return: 0 成功，-1 失败
 */
int vmm_map_page(uint64_t phys, uint64_t virt, uint64_t flags);

/**
 * vmm_unmap_page - 解除虚拟地址的映射
 * @virt:   虚拟地址（必须页对齐）
 */
void vmm_unmap_page(uint64_t virt);

/**
 * vmm_get_phys - 获取虚拟地址对应的物理地址
 * @virt:   虚拟地址
 * @return: 物理地址，0 表示未映射
 */
uint64_t vmm_get_phys(uint64_t virt);

/**
 * vmm_change_flags - 修改已映射页的属性
 * @virt:   虚拟地址（必须页对齐）
 * @flags:  新的页表项标志
 */
void vmm_change_flags(uint64_t virt, uint64_t flags);

/**
 * vmm_is_mapped - 检查虚拟地址是否已映射
 * @virt:   虚拟地址
 * @return: true 已映射，false 未映射
 */
bool vmm_is_mapped(uint64_t virt);

/**
 * vmm_alloc_map - 分配一个物理页并映射到指定虚拟地址
 * @virt:   虚拟地址（必须页对齐）
 * @flags:  页表项标志
 * @return: 0 成功，-1 失败
 */
int vmm_alloc_map(uint64_t virt, uint64_t flags);

/**
 * vmm_map_mmio - 将物理 MMIO 地址映射到内核专用 MMIO 区域
 * @phys_addr:  MMIO 物理地址（会自动对齐到页）
 * @size:       需要映射的字节数
 * @return:     映射后的虚拟地址，0 表示失败
 */
uint64_t vmm_map_mmio(uint64_t phys_addr, size_t size);

/**
 * vmm_invalidate_page - 使单个 TLB 条目失效
 * @addr:   虚拟地址
 */
static inline void vmm_invalidate_page(uint64_t addr)
{
    __asm__ volatile ("invlpg (%0)" :: "r"(addr) : "memory");
}

/**
 * vmm_invalidate_all - 刷新整个 TLB（重载 CR3）
 */
static inline void vmm_invalidate_all(void)
{
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

#endif /* SUKIOS_VMM_H */
