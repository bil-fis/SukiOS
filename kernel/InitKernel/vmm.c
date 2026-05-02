/* =============================================================================
 * SukiOS - 虚拟内存管理器实现（VMM）
 *
 * 参考：OSDev Paging, Intel SDM Vol.3 Chapter 4
 *
 * 页表层次结构（x86_64 4 级分页）：
 *   PML4 (Page Map Level 4) -> PDPT -> PD -> PT -> 物理页
 *   每级 512 个 64 位条目，每张表占 4KB
 *
 * 初始化流程：
 *   1. 分配新的 PML4
 *   2. 高半区：使用 2MB 大页映射 512MB 内核空间
 *   3. 低地址：保留 0-4GB 身份映射（1GB 大页 × 4）
 *   4. 激活新页表（加载 CR3）
 * ============================================================================= */

#include "kernel/vmm.h"
#include "kernel/pmm.h"
#include "kernel/tty.h"
#include "kernel/io.h"

/* ---- 当前活跃 PML4 的物理地址 ---- */
static uint64_t s_pml4_phys = 0;

/* ---- 当前活跃 PML4 的虚拟地址（通过临时身份映射访问） ---- */
static pml4e_t *s_pml4_virt = NULL;

/* ---- 临时映射用的 MMIO 虚拟地址游标 ---- */
static uint64_t s_mmio_next = VMM_MMIO_BASE;

/* ---- 内核虚拟基址 ---- */
#define KERNEL_VMA  0xFFFFFFFF80000000ULL

/* =========================================================================
 * 辅助：从 PML4 物理地址获取层级页表的虚拟地址
 *
 * 页表本身是物理页，需要通过身份映射或固定偏移来访问。
 * 我们使用 boot.asm 建立的 0-4GB 身份映射来访问物理页。
 * ========================================================================= */

/* 获取 PDPTE 的虚拟地址（通过身份映射访问物理内存） */
static pdpte_t *get_pdpte(uint64_t vaddr, bool alloc)
{
    uint64_t pml4_idx = vmm_pml4_index(vaddr);
    pml4e_t entry = s_pml4_virt[pml4_idx];

    if (!(entry & VMM_PRESENT)) {
        if (!alloc) return NULL;

        /* 分配新的 PDPTE 页 */
        uint64_t phys = pmm_alloc_page();
        if (!phys) return NULL;

        /* 清零新页表 */
        uint64_t *page = (uint64_t *)(uintptr_t)phys;
        for (int i = 0; i < 512; i++)
            page[i] = 0;

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

    /* 检查是否为 1GB 大页 */
    if (entry & VMM_PS) return NULL;

    if (!(entry & VMM_PRESENT)) {
        if (!alloc) return NULL;

        uint64_t phys = pmm_alloc_page();
        if (!phys) return NULL;

        uint64_t *page = (uint64_t *)(uintptr_t)phys;
        for (int i = 0; i < 512; i++)
            page[i] = 0;

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

    /* 检查是否为 2MB 大页 */
    if (entry & VMM_PS) return NULL;

    if (!(entry & VMM_PRESENT)) {
        if (!alloc) return NULL;

        uint64_t phys = pmm_alloc_page();
        if (!phys) return NULL;

        uint64_t *page = (uint64_t *)(uintptr_t)phys;
        for (int i = 0; i < 512; i++)
            page[i] = 0;

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
    /* 确保页对齐 */
    phys &= ~(VMM_PAGE_SIZE - 1);
    virt &= ~(VMM_PAGE_SIZE - 1);

    pte_t *pt = get_pt(virt, true);
    if (!pt) return -1;

    uint64_t pt_idx = vmm_pt_index(virt);
    pt[pt_idx] = phys | (flags & ~VMM_PAGE_SIZE);

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

    /* 保留物理地址，只修改标志位 */
    pt[pt_idx] = (old_entry & 0x000FFFFFFFFFF000ULL) | (flags & ~VMM_PAGE_SIZE);

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

    /* 清零物理页（通过身份映射访问） */
    uint64_t *page = (uint64_t *)(uintptr_t)phys;
    for (int i = 0; i < 512; i++)
        page[i] = 0;

    return vmm_map_page(phys, virt, flags);
}

uint64_t vmm_map_mmio(uint64_t phys_addr, size_t size)
{
    /* 页对齐 */
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
 *
 * 从 boot.asm 的临时页表迁移到正式内核页表。
 *
 * boot.asm 建立的页表：
 *   PML4[0]    -> PDPTE_A -> 1GB 大页 × 4（0-4GB 身份映射）
 *   PML4[511]  -> PDPTE_B -> PD_B -> PT_B（KERNEL_VIRT, 2MB）
 *
 * 新页表布局：
 *   PML4[0]    -> PDPTE_A -> 1GB 大页 × 4（保留 0-4GB 身份映射）
 *   PML4[511]  -> 新 PDPTE -> 2MB 大页 × 256（覆盖 512MB 高半区）
 *   PML4[510]  -> 新 PDPTE -> 用于 MMIO 映射和内核堆
 * ========================================================================= */

/* ---- 页表物理地址（与 boot.asm 一致） ---- */
#define BOOT_PML4_ADDR    0x70000
#define BOOT_PDPTE_A_ADDR 0x71000
#define BOOT_PDPTE_B_ADDR 0x74000

void vmm_init(void)
{
    tty_print("[..] Initializing Virtual Memory Manager...\n");

    /* 获取当前 CR3（boot.asm 建立的 PML4） */
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    s_pml4_phys = cr3;
    s_pml4_virt = (pml4e_t *)(uintptr_t)cr3;

    /* ---- 1. 扩展身份映射到 0-4GB ----
     *
     * boot.asm 的 PDPTE_A 原始设置：PDPTE_A[0] -> PD_A -> PT_A（4KB 页，0-4MB）
     * 现在改为 4 个 1GB 大页，覆盖 0-4GB 完整身份映射。
     * 这样 MMIO 区域（APIC 0xFEE00000、ACPI 表等）都可访问。
     *
     * 内核在高半区运行（PML4[511]），不依赖低地址的 4KB 精细映射，
     * 所以覆盖 PDPTE_A[0] 是安全的。
     *
     * flags: P=1(bit0) + RW=1(bit1) + PS=1(bit7) = 0x83
     * 参考：Intel SDM Vol.3 Figure 4-5: 1-GByte Page Table Entry */
    volatile uint64_t *pdpte_a = (volatile uint64_t *)(uintptr_t)BOOT_PDPTE_A_ADDR;
    for (int i = 0; i < 4; i++) {
        uint64_t phys = (uint64_t)i * 0x40000000ULL;  /* i * 1GB */
        pdpte_a[i] = phys | VMM_PRESENT | VMM_WRITE | VMM_PS;
    }

    /* ---- 2. 刷新 TLB ---- */
    vmm_invalidate_all();

    /* ---- 3. 确保内核 PDPTE（PDPTE_B）可扩展 ----
     *
     * boot.asm 的映射链：PML4[511] -> PDPTE_B -> PD_B -> PT_B（内核 4KB 页）
     * PDPTE_B[510] = PD_B（已设置，内核代码）
     *
     * 堆区域 VMM_HEAP_BASE 和 MMIO 区域 VMM_MMIO_BASE 都在 PML4[511] 下：
     *   KERNEL_VMA    (0xFFFFFFFF80000000): PDPTE[510], PD[0]
     *   VMM_HEAP_BASE (0xFFFFFFFFA0000000): PDPTE[510], PD[256]
     *   VMM_MMIO_BASE (0xFFFFFFFFC0000000): PDPTE[511]
     *
     * VMM 的 get_pdpte/get_pd/get_pt 会通过身份映射遍历页表层级，
     * 当发现 PDPTE[511] 未设置时自动分配 PD 表。
     * 无需在此预分配，vmm_alloc_map / vmm_map_page 会按需创建。
     */

    tty_print("[OK] VMM initialized\n");
    tty_print("  PML4 at phys=");
    tty_print_hex64(s_pml4_phys);
    tty_print("\n");
    tty_print("  Identity map: 0-4GB (1GB pages)\n");
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
