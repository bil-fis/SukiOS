/*
 * kernel/mm/vmm.c
 * -----------------------------------------------------------------------------
 * 4 级分页虚拟内存管理器。
 *
 * 地址空间约定：内核 PML4 沿用引导期建立的页表（CR3）。所有页表内容通过
 * PHYS_TO_VIRT() 访问（内核高半区已映射低 4GB 物理内存）。
 *
 * 用户地址空间：vmm_create_address_space() 复制内核 PML4 的高半区项
 * (索引 256..511) 以共享内核映射，用户半区(0..255)留空。
 *
 * 调用关系：kmain() -> vmm_init(); 进程/kmalloc -> vmm_map_page()。
 */
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <kernel/string.h>
#include <kernel/console.h>

static uint64_t g_kernel_pml4;   /* 物理地址 */

/* 读取 CR3 */
static __attribute__((noinline)) uint64_t read_cr3(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

/* 写入 CR3（切换地址空间） */
static __attribute__((noinline)) void write_cr3(uint64_t v)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory");
}

/* 使单个 TLB 项失效 */
static __attribute__((noinline)) void invlpg(uint64_t virt)
{
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

static uint64_t *table_at(uint64_t table_phys)
{
    return (uint64_t *)PHYS_TO_VIRT(table_phys & PTE_ADDR_MASK);
}

/* 取/建下一级页表，返回下一级物理地址；user 决定中间项是否带 USER 位 */
static uint64_t next_level(uint64_t *table, size_t idx, bool create, bool user)
{
    if (table[idx] & PTE_PRESENT) {
        return table[idx] & PTE_ADDR_MASK;
    }
    if (!create) {
        return 0;
    }
    void *page = pmm_alloc_page();
    if (!page) {
        return 0;
    }
    uint64_t flags = PTE_PRESENT | PTE_WRITE;
    if (user) {
        flags |= PTE_USER;
    }
    table[idx] = ((uint64_t)page & PTE_ADDR_MASK) | flags;
    return (uint64_t)page;
}

static void split_indices(uint64_t virt, size_t *i4, size_t *i3, size_t *i2, size_t *i1)
{
    *i4 = (virt >> 39) & 0x1FF;
    *i3 = (virt >> 30) & 0x1FF;
    *i2 = (virt >> 21) & 0x1FF;
    *i1 = (virt >> 12) & 0x1FF;
}

bool vmm_map_page(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags)
{
    size_t i4, i3, i2, i1;
    split_indices(virt, &i4, &i3, &i2, &i1);
    bool user = (flags & PTE_USER) != 0;

    uint64_t *pml4 = table_at(pml4_phys);
    uint64_t pdpt_phys = next_level(pml4, i4, true, user);
    if (!pdpt_phys) return false;

    uint64_t *pdpt = table_at(pdpt_phys);
    uint64_t pd_phys = next_level(pdpt, i3, true, user);
    if (!pd_phys) return false;

    uint64_t *pd = table_at(pd_phys);
    uint64_t pt_phys = next_level(pd, i2, true, user);
    if (!pt_phys) return false;

    uint64_t *pt = table_at(pt_phys);
    pt[i1] = (phys & PTE_ADDR_MASK) | (flags & ~PTE_ADDR_MASK) | PTE_PRESENT;
    invlpg(virt);
    return true;
}

void vmm_unmap_page(uint64_t pml4_phys, uint64_t virt)
{
    size_t i4, i3, i2, i1;
    split_indices(virt, &i4, &i3, &i2, &i1);

    uint64_t *pml4 = table_at(pml4_phys);
    if (!(pml4[i4] & PTE_PRESENT)) return;
    uint64_t *pdpt = table_at(pml4[i4] & PTE_ADDR_MASK);
    if (!(pdpt[i3] & PTE_PRESENT)) return;
    uint64_t *pd = table_at(pdpt[i3] & PTE_ADDR_MASK);
    if (!(pd[i2] & PTE_PRESENT)) return;
    uint64_t *pt = table_at(pd[i2] & PTE_ADDR_MASK);
    pt[i1] = 0;
    invlpg(virt);
}

uint64_t vmm_translate(uint64_t pml4_phys, uint64_t virt)
{
    size_t i4, i3, i2, i1;
    split_indices(virt, &i4, &i3, &i2, &i1);

    uint64_t *pml4 = table_at(pml4_phys);
    if (!(pml4[i4] & PTE_PRESENT)) return 0;
    uint64_t *pdpt = table_at(pml4[i4] & PTE_ADDR_MASK);
    if (!(pdpt[i3] & PTE_PRESENT)) return 0;
    if (pdpt[i3] & PTE_HUGE) {
        return (pdpt[i3] & PTE_ADDR_MASK) + (virt & 0x3FFFFFFF);   /* 1GB 页 */
    }
    uint64_t *pd = table_at(pdpt[i3] & PTE_ADDR_MASK);
    if (!(pd[i2] & PTE_PRESENT)) return 0;
    if (pd[i2] & PTE_HUGE) {
        return (pd[i2] & PTE_ADDR_MASK) + (virt & 0x1FFFFF);       /* 2MB 页 */
    }
    uint64_t *pt = table_at(pd[i2] & PTE_ADDR_MASK);
    if (!(pt[i1] & PTE_PRESENT)) return 0;
    return (pt[i1] & PTE_ADDR_MASK) + (virt & 0xFFF);
}

/* 返回页表项原始值（含标志位），未映射返回 0 —— 供 copy_*_user 做权限校验 */
uint64_t vmm_pte(uint64_t pml4_phys, uint64_t virt)
{
    size_t i4, i3, i2, i1;
    split_indices(virt, &i4, &i3, &i2, &i1);

    uint64_t *pml4 = table_at(pml4_phys);
    if (!(pml4[i4] & PTE_PRESENT)) return 0;
    uint64_t *p3 = table_at(pml4[i4] & PTE_ADDR_MASK);
    if (!(p3[i3] & PTE_PRESENT)) return 0;
    if (p3[i3] & PTE_HUGE) return p3[i3];
    uint64_t *p2 = table_at(p3[i3] & PTE_ADDR_MASK);
    if (!(p2[i2] & PTE_PRESENT)) return 0;
    if (p2[i2] & PTE_HUGE) return p2[i2];
    uint64_t *pt = table_at(p2[i2] & PTE_ADDR_MASK);
    if (!(pt[i1] & PTE_PRESENT)) return 0;
    return pt[i1];
}

uint64_t vmm_create_address_space(void)
{
    void *pml4_page = pmm_alloc_page();
    if (!pml4_page) {
        return 0;
    }
    uint64_t new_pml4 = (uint64_t)pml4_page;
    uint64_t *dst = table_at(new_pml4);
    uint64_t *src = table_at(g_kernel_pml4);

    /* 用户半区(0..255)清零，内核半区(256..511)共享内核映射 */
    for (int i = 0; i < 256; i++) {
        dst[i] = 0;
    }
    for (int i = 256; i < 512; i++) {
        dst[i] = src[i];
    }
    return new_pml4;
}

void vmm_switch(uint64_t pml4_phys)
{
    write_cr3(pml4_phys & PTE_ADDR_MASK);
}

uint64_t vmm_kernel_pml4(void)
{
    return g_kernel_pml4;
}

/* 把 [0, highest) 全部物理内存映射到内核高半区（替换引导期临时 2MB 大页
 * 仅覆盖 0..4GB 的限制）。对 4GB 以上每 1GB 块分配一个 PD 页并以 2MB 大页
 * 映射，同时保持恒等映射(pml4[0])与高半区映射(pml4[256])一致（B4 项）。 */
void vmm_extend_kernel_mapping(uint64_t highest)
{
    uint64_t *pml4 = table_at(g_kernel_pml4);
    uint64_t max_addr = highest;
    if (max_addr > (uint64_t)512 * 1024 * 1024 * 1024)
        max_addr = (uint64_t)512 * 1024 * 1024 * 1024;   /* 单 PML3 上限 512GB */

    for (uint64_t gb = 4UL * 1024 * 1024 * 1024; gb < max_addr; gb += (1UL << 30)) {
        for (int half = 0; half < 2; half++) {
            size_t p4idx = (half == 0) ? 0 : 256;
            uint64_t *p3 = table_at(pml4[p4idx] & PTE_ADDR_MASK);
            size_t p3i = (gb >> 30) & 0x1FF;
            uint64_t p2_phys = p3[p3i] & PTE_ADDR_MASK;
            if (!(p3[p3i] & PTE_PRESENT)) {
                void *pg = pmm_alloc_page();
                if (!pg) return;                 /* 内存耗尽：保留已映射部分 */
                p2_phys = (uint64_t)pg;
                p3[p3i] = p2_phys | PTE_PRESENT | PTE_WRITE;
            }
            uint64_t *p2 = table_at(p2_phys);
            for (int i = 0; i < 512; i++) {
                uint64_t pa = gb + (uint64_t)i * (2UL << 20);
                if (pa >= max_addr) break;
                p2[i] = (pa & PTE_ADDR_MASK) | PTE_PRESENT | PTE_WRITE | PTE_HUGE;
            }
        }
    }
    kprintf("[vmm] extended kernel mapping up to %lu MiB of RAM\n",
            (unsigned long)(max_addr / (1024 * 1024)));
}

/* 销毁用户地址空间：释放用户半区(0..255)全部页表结构，并释放“自有”物理页；
 * 带 PTE_OOL 标记的页（IPC 共享页）只解除映射，引用计数由 OOL 清理路径处理。 */
void vmm_destroy_address_space(uint64_t pml4_phys)
{
    uint64_t *pml4 = table_at(pml4_phys);
    for (int i4 = 0; i4 < 256; i4++) {
        if (!(pml4[i4] & PTE_PRESENT)) continue;
        uint64_t *p3 = table_at(pml4[i4] & PTE_ADDR_MASK);
        for (int i3 = 0; i3 < 512; i3++) {
            if (!(p3[i3] & PTE_PRESENT)) continue;
            uint64_t *p2 = table_at(p3[i3] & PTE_ADDR_MASK);
            for (int i2 = 0; i2 < 512; i2++) {
                if (!(p2[i2] & PTE_PRESENT)) continue;
                if (p2[i2] & PTE_HUGE) { p2[i2] = 0; continue; }
                uint64_t *pt = table_at(p2[i2] & PTE_ADDR_MASK);
                for (int i1 = 0; i1 < 512; i1++) {
                    if (pt[i1] & PTE_PRESENT) {
                        if (!(pt[i1] & PTE_OOL))
                            pmm_free_page((void *)(pt[i1] & PTE_ADDR_MASK));
                        pt[i1] = 0;
                    }
                }
                pmm_free_page((void *)(p2[i2] & PTE_ADDR_MASK));
            }
            pmm_free_page((void *)(p3[i3] & PTE_ADDR_MASK));
        }
        pmm_free_page((void *)(pml4[i4] & PTE_ADDR_MASK));
    }
    pmm_free_page((void *)(pml4_phys & PTE_ADDR_MASK));
}

void vmm_init(void)
{
    g_kernel_pml4 = read_cr3() & PTE_ADDR_MASK;
    /* 扩展内核映射以覆盖全部 RAM（>4GB 支持，B4） */
    vmm_extend_kernel_mapping(pmm_total_pages() * PAGE_SIZE);
    kprintf("[vmm] kernel PML4 @ phys %p (4-level paging active)\n",
            (void *)g_kernel_pml4);
}
