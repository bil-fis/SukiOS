/* =============================================================================
 * SukiOS - 虚拟内存管理器实现（VMM）
 *
 * 参考：OSDev Paging, Intel SDM Vol.3 Chapter 4
 *
 * 页表层次结构（x86_64 4 级分页）：
 *   PML4 -> PDPT -> PD -> PT -> 物理页
 *
 * 关键修正：
 * - 新分配页表页使用逐字节清零，彻底消除高位残留。
 * - vmm_map_page / vmm_change_flags 强制掩码物理地址和标志位。
 * ============================================================================= */

#include "kernel/vmm.h"
#include "kernel/pmm.h"
#include "kernel/tty.h"
#include <stddef.h>   /* for size_t, memset prototype */

/* ---- 当前活跃 PML4 的物理地址 ---- */
static uint64_t s_pml4_phys = 0;
static pml4e_t *s_pml4_virt = NULL;
static uint64_t s_mmio_next = VMM_MMIO_BASE;

#define KERNEL_VMA  0xFFFFFFFF80000000ULL

/* ---- 安全清零一整个物理页（4KB） ---- */
static inline void clear_page(uint64_t phys)
{
    /* 通过身份映射清零该页的所有字节 */
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)phys;
    for (size_t i = 0; i < VMM_PAGE_SIZE; i++)
        p[i] = 0;
}

/* 获取 PDPTE 的虚拟地址 */
static pdpte_t *get_pdpte(uint64_t vaddr, bool alloc)
{
    uint64_t pml4_idx = vmm_pml4_index(vaddr);
    pml4e_t entry = s_pml4_virt[pml4_idx];

    if (!(entry & VMM_PRESENT)) {
        if (!alloc) return NULL;

        uint64_t phys = pmm_alloc_page();
        if (!phys) return NULL;

        clear_page(phys);                       /* 彻底清零 */
        s_pml4_virt[pml4_idx] = phys | VMM_PRESENT | VMM_WRITE;
        entry = s_pml4_virt[pml4_idx];
    }

    return (pdpte_t *)(uintptr_t)(entry & 0x000FFFFFFFFFF000ULL);
}

/* 获取 PD 的虚拟地址 */
static pde_t *get_pd(uint64_t vaddr, bool alloc)
{
    pdpte_t *pdpte = get_pdpte(vaddr, alloc);
    if (!pdpte) return NULL;

    uint64_t pdpt_idx = vmm_pdpte_index(vaddr);
    pdpte_t entry = pdpte[pdpt_idx];

    if (entry & VMM_PS) return NULL;   /* 1GB 大页 */

    if (!(entry & VMM_PRESENT)) {
        if (!alloc) return NULL;

        uint64_t phys = pmm_alloc_page();
        if (!phys) return NULL;

        clear_page(phys);                       /* 彻底清零 */
        pdpte[pdpt_idx] = phys | VMM_PRESENT | VMM_WRITE;
        entry = pdpte[pdpt_idx];
    }

    return (pde_t *)(uintptr_t)(entry & 0x000FFFFFFFFFF000ULL);
}

/* 获取 PT 的虚拟地址 */
static pte_t *get_pt(uint64_t vaddr, bool alloc)
{
    pde_t *pd = get_pd(vaddr, alloc);
    if (!pd) return NULL;

    uint64_t pd_idx = vmm_pd_index(vaddr);
    pde_t entry = pd[pd_idx];

    if (entry & VMM_PS) return NULL;   /* 2MB 大页 */

    if (!(entry & VMM_PRESENT)) {
        if (!alloc) return NULL;

        uint64_t phys = pmm_alloc_page();
        if (!phys) return NULL;

        clear_page(phys);                       /* 彻底清零 */
        pd[pd_idx] = phys | VMM_PRESENT | VMM_WRITE;
        entry = pd[pd_idx];
    }

    return (pte_t *)(uintptr_t)(entry & 0x000FFFFFFFFFF000ULL);
}

/* =========================================================================
 * 核心接口实现
 * ========================================================================= */

int vmm_map_page(uint64_t phys, uint64_t virt, uint64_t flags)
{
    phys &= ~(VMM_PAGE_SIZE - 1);
    virt &= ~(VMM_PAGE_SIZE - 1);

    /* ---- 第一步：拿到 PD 并尝试正常获取 PT ---- */
    pde_t *pd = get_pd(virt, true);
    if (!pd) return -1;

    uint64_t pd_idx = vmm_pd_index(virt);
    pde_t pde = pd[pd_idx];

    /* 如果当前是 2MB 大页 */
    if (pde & VMM_PS) {
        /* 分配新页作为页表 (PT) */
        uint64_t pt_phys = pmm_alloc_page();
        if (!pt_phys) return -1;
        clear_page(pt_phys);

        /* 获取大页的物理基址 */
        uint64_t base_phys = pde & 0x000FFFFFFFE00000ULL; /* 2MB 对齐 */

        /* 在页表中填入 512 个 4KB 页，权限沿用原大页的 */
        volatile pte_t *pt = (volatile pte_t *)(uintptr_t)pt_phys;
        for (int i = 0; i < 512; i++) {
            pt[i] = (base_phys + i * VMM_PAGE_SIZE) | (pde & 0xFFF) | VMM_PRESENT;
        }

        /* 更新 PDE，指向新 PT，清除 PS 位 */
        pd[pd_idx] = pt_phys | VMM_PRESENT | VMM_WRITE | (pde & VMM_USER);  /* PT 本身必须可读写 */
        vmm_invalidate_all();  /* 刷新 TLB，确保后续访问通过新 PT */
    }

    /* ---- 第二步：正常拿到 PT，设置新映射 ---- */
    pte_t *pt = get_pt(virt, true);
    if (!pt) return -1;

    uint64_t pt_idx = vmm_pt_index(virt);
    pt[pt_idx] = (phys & 0x000FFFFFFFFFF000ULL) | ((flags & 0xFFFULL) & ~VMM_XD);

    vmm_invalidate_page(virt);
    return 0;
}

void vmm_unmap_page(uint64_t virt)
{
    virt &= ~(VMM_PAGE_SIZE - 1);

    pte_t *pt = get_pt(virt, false);
    if (!pt) return;

    uint64_t pt_idx = vmm_pt_index(virt);
    pt[pt_idx] = 0;
    vmm_invalidate_page(virt);
}

uint64_t vmm_get_phys(uint64_t virt)
{
    pte_t *pt = get_pt(virt, false);
    if (!pt) return 0;

    uint64_t pt_idx = vmm_pt_index(virt);
    pte_t entry = pt[pt_idx];

    if (!(entry & VMM_PRESENT)) return 0;
    return (entry & 0x000FFFFFFFFFF000ULL) | vmm_page_offset(virt);
}

void vmm_change_flags(uint64_t virt, uint64_t flags)
{
    virt &= ~(VMM_PAGE_SIZE - 1);

    pte_t *pt = get_pt(virt, false);
    if (!pt) return;

    uint64_t pt_idx = vmm_pt_index(virt);
    uint64_t old_entry = pt[pt_idx];

    /* 保留物理地址，合并新标志，并强制清除 XD 位 */
    pt[pt_idx] = ((old_entry & 0x000FFFFFFFFFF000ULL) | (flags & 0xFFFULL)) & ~VMM_XD;

    vmm_invalidate_page(virt);
}

bool vmm_is_mapped(uint64_t virt)
{
    pte_t *pt = get_pt(virt, false);
    if (!pt) return false;

    uint64_t pt_idx = vmm_pt_index(virt);
    return (pt[pt_idx] & VMM_PRESENT) != 0;
}

int vmm_alloc_map(uint64_t virt, uint64_t flags)
{
    virt &= ~(VMM_PAGE_SIZE - 1);

    uint64_t phys = pmm_alloc_page();
    if (!phys) return -1;

    /* 清零物理页 */
    clear_page(phys);

    return vmm_map_page(phys, virt, flags);
}

uint64_t vmm_map_mmio(uint64_t phys_addr, size_t size)
{
    uint64_t phys_start = phys_addr & ~(VMM_PAGE_SIZE - 1);
    uint64_t phys_end   = (phys_addr + size + VMM_PAGE_SIZE - 1) & ~(VMM_PAGE_SIZE - 1);
    size_t page_count   = (phys_end - phys_start) / VMM_PAGE_SIZE;

    uint64_t virt_base = s_mmio_next;

    for (size_t i = 0; i < page_count; i++) {
        uint64_t virt = s_mmio_next;
        if (vmm_map_page(phys_start + i * VMM_PAGE_SIZE, virt, VMM_KERN_RW_CD) != 0) {
            return 0;
        }
        s_mmio_next += VMM_PAGE_SIZE;
    }

    return virt_base + (phys_addr & 0xFFF);
}

/* =========================================================================
 * VMM 初始化
 * ========================================================================= */
#define BOOT_PML4_ADDR    0x70000
#define BOOT_PDPTE_A_ADDR 0x71000
#define BOOT_PDPTE_B_ADDR 0x74000

void vmm_init(void)
{
    tty_print("[..] Initializing Virtual Memory Manager...\n");

    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    s_pml4_phys = cr3;
    s_pml4_virt = (pml4e_t *)(uintptr_t)cr3;

    /* 确保 PML4[0] 允许用户访问（指向 PDPTE_A） */
    s_pml4_virt[0] |= VMM_USER;

    /* 1. 扩展身份映射到 0-4GB，使用 2MB 大页，并设置用户权限 */
    volatile pdpte_t *pdpte_a = (volatile pdpte_t *)(uintptr_t)BOOT_PDPTE_A_ADDR;
    for (int gb = 0; gb < 4; gb++) {
        uint64_t pd_phys = pmm_alloc_page();
        if (!pd_phys) {
            tty_setcolor(VGA_RED, VGA_BLACK);
            tty_print("  ERROR: Failed to allocate PD for identity map\n");
            return;
        }
        volatile pde_t *pd = (volatile pde_t *)(uintptr_t)pd_phys;
        for (int i = 0; i < 512; i++) {
            uint64_t phys = (uint64_t)(gb * 0x40000000ULL) + (i * 0x200000ULL);
            pd[i] = phys | VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_PS;
        }
        pdpte_a[gb] = pd_phys | VMM_PRESENT | VMM_WRITE | VMM_USER;
    }

    vmm_invalidate_all();

    tty_print("[OK] VMM initialized\n");
    tty_print("  PML4 at phys=");
    tty_print_hex64(s_pml4_phys);
    tty_print("\n");
    tty_print("  Identity map: 0-4GB (2MB pages, user accessible)\n");
    tty_print("  Kernel: ");
    tty_print_hex64(KERNEL_VMA);
    tty_print(" (boot.asm mapping preserved)\n");
    tty_print("  Heap:   ");
    tty_print_hex64(VMM_HEAP_BASE);
    tty_print(" (on-demand via PML4[511])\n");
    tty_print("  MMIO:   ");
    tty_print_hex64(VMM_MMIO_BASE);
    tty_print(" (on-demand via PML4[511])\n");
}