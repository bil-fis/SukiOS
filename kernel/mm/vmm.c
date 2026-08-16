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
#include <kernel/smp.h>
#include <kernel/security.h>   /* IST_GUARD_BASE/IST_VA：KPTI 影子映射 IST 栈 */

static uint64_t g_kernel_pml4;   /* 物理地址 */

/* P0-8 KASLR：高半区基址随机滑动量（字节，1GB 对齐）。由 boot.S 在引导期
 * （切页表前、经旧基址映射）写入，供 security.c 报告与诊断路径使用。 */
uint64_t g_kernel_slide = 0;

/* P0-8 KASLR：运行期高半区实际基址 = KERNEL_BASE + g_kernel_slide。
 * PHYS_TO_VIRT/VIRT_TO_PHYS（types.h）以此为偏移。必须放 .data（带初值）：
 * 若放 .bss 且 boot.S 未写（如构建异常），至少退化为固定基址仍可启动。
 * boot.S 在应用重定位后、切 CR3 前写入随机值。 */
uint64_t g_virt_base = KERNEL_BASE;

/* P0-R2 KPTI：全局开关。security_init 依 IA32_ARCH_CAPABILITIES.RDCL_NO
 * 检测置位（不免疫/未知 -> 1）。置位时刻在任何会进入 Ring3 的地址空间创建
 * 之前（kmain: security_init 先于 sched_init/task_create_user），且此后
 * 永不改变——vmm_create/destroy 据此判断地址空间是否成对，二者必须见到
 * 同一值（mm_selftest 的临时空间创建/销毁均在置位前，同样自洽）。
 * syscall_entry.S / isr.S / enter_user_mode 以字节读取（movabsq 取址，
 * KASLR 重定位循环自动修补）。 */
uint8_t g_kpti_enabled = 0;

/* 链接脚本符号：内核映像分段边界（KPTI 影子映射按段赋权） */
extern char __text_start[], __text_end[];
extern char __rodata_start[], __rodata_end[];
extern char __data_start[];
extern char __kernel_end[];

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

/* P0-R2 KPTI：把用户地址空间「内核视图 PML4」的用户半区第 i4 项同步进
 * 影子 PML4（影子与内核视图共享同一批用户半区下级页表——PDPT 及以下的
 * 增删改自动对两个视图生效，唯 PML4 顶层项新建时需要手工镜像一次）。
 * 判据：g_kpti_enabled（启用后创建的空间必成对）且非内核 PML4 且 i4<256。 */
static void kpti_sync_user_slot(uint64_t pml4_phys, size_t i4)
{
    if (!g_kpti_enabled || pml4_phys == g_kernel_pml4 || i4 >= 256) {
        return;
    }
    uint64_t *kview  = table_at(pml4_phys);
    uint64_t *shadow = table_at(pml4_phys + PAGE_SIZE);
    shadow[i4] = kview[i4];
}

bool vmm_map_page(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags)
{
    size_t i4, i3, i2, i1;
    split_indices(virt, &i4, &i3, &i2, &i1);
    bool user = (flags & PTE_USER) != 0;

    uint64_t *pml4 = table_at(pml4_phys);
    uint64_t pdpt_phys = next_level(pml4, i4, true, user);
    if (!pdpt_phys) return false;
    kpti_sync_user_slot(pml4_phys, i4);   /* 顶层项可能刚新建，镜像到影子 */

    uint64_t *pdpt = table_at(pdpt_phys);
    uint64_t pd_phys = next_level(pdpt, i3, true, user);
    if (!pd_phys) return false;

    uint64_t *pd = table_at(pd_phys);
    uint64_t pt_phys = next_level(pd, i2, true, user);
    if (!pt_phys) return false;

    uint64_t *pt = table_at(pt_phys);
    if (pt[i1] & PTE_PRESENT) {
        /* M3 修复：重复映射同一虚拟页。原实现直接覆盖 PTE，旧物理页既不释放
         * （内核页泄漏）又可能静默降级权限。这里先释放旧物理页（OOL 共享页
         * 由引用计数管理，不在此释放），再建立新映射，使语义明确。
         * ELF 多段页边界重叠等场景因此不再泄漏内核页。 */
        uint64_t old = pt[i1] & PTE_ADDR_MASK;
        if (!(pt[i1] & PTE_OOL)) {
            pmm_free_page((void *)old);
        } else {
            kprintf("[vmm] remap over OOL page va=%p (refcount retained)\n",
                    (void *)virt);
        }
    }
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
    /* P0-3：内核高半区映射被所有 CPU 共享（AP 亦缓存其 TLB 项），收回内核
     * 映射必须广播 shootdown；用户半区当前仅 BSP 调度，本地 invlpg 已足够。 */
    if (virt >= KERNEL_BASE) {
        smp_tlb_shootdown();
    }
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

/* ========================================================================= */
/*  P0-R2 KPTI：影子页表（KAISER 式最小内核窗口）                              */
/* ========================================================================= */

/* 在影子 PML4 的【内核半区】建立一个 4KB 映射。与 vmm_map_page 的区别：
 *   - 中间页表全部新建于影子树内（绝不复用内核 PML4 的下级表——否则会把
 *     完整内核映射带进影子，KPTI 形同虚设）；
 *   - 中间项不带 PTE_USER（内核窗口 Ring3 不可见，仅供 CPL0 入口路径取指/
 *     压栈；Meltdown 泄露面被压缩到映像+栈，物理直映区完全不可达）；
 *   - 叶项已存在时直接覆写（同一页被重复登记是幂等操作）。 */
static bool shadow_map_page(uint64_t shadow_pml4, uint64_t virt,
                            uint64_t phys, uint64_t flags)
{
    size_t i4, i3, i2, i1;
    split_indices(virt, &i4, &i3, &i2, &i1);

    uint64_t *pml4 = table_at(shadow_pml4);
    uint64_t pdpt_phys = next_level(pml4, i4, true, false);
    if (!pdpt_phys) return false;
    uint64_t *pdpt = table_at(pdpt_phys);
    uint64_t pd_phys = next_level(pdpt, i3, true, false);
    if (!pd_phys) return false;
    uint64_t *pd = table_at(pd_phys);
    uint64_t pt_phys = next_level(pd, i2, true, false);
    if (!pt_phys) return false;
    uint64_t *pt = table_at(pt_phys);
    pt[i1] = (phys & PTE_ADDR_MASK) | (flags & ~PTE_ADDR_MASK) | PTE_PRESENT;
    return true;
}

/* 把一段【内核直映/高半区】虚拟区间逐页映射进影子（VA -> 经内核 PML4 翻译
 * 的物理页）。区间端点自动页对齐（base 向下、end 向上）。未映射页跳过。 */
static void shadow_map_range(uint64_t shadow_pml4, uint64_t va_start,
                             uint64_t va_end, uint64_t flags)
{
    uint64_t va = va_start & ~(PAGE_SIZE - 1);
    uint64_t end = (va_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (; va < end; va += PAGE_SIZE) {
        uint64_t pa = vmm_translate(g_kernel_pml4, va);
        if (!pa) {
            continue;   /* 空洞（如尚未建立的 IST 栈）：跳过 */
        }
        shadow_map_page(shadow_pml4, va, pa & ~(PAGE_SIZE - 1), flags);
    }
}

/* 填充影子 PML4 的最小内核窗口：
 *   1) 内核映像三段（按段赋权：.text RX / .rodata RO+NX / .data+.bss RW+NX）。
 *      入口路径的全部代码（syscall_entry/isr 存根）、数据（IDT/GDT/TSS/
 *      g_percpu/g_syscall_kstack/g_kpti_enabled）都落在映像内；
 *   2) IST 守卫栈映射页（#DF/NMI 从 Ring3 进入时 CPU 直接向 IST 压栈，
 *      影子里必须可写；守卫页刻意不映射，保留溢出即 #PF 语义）。
 *   注意（KASLR 自洽）：__text_start 等符号引用经 movabsq 绝对寻址，boot 期
 *   重定位循环已修补为「随机滑动后」的运行期 VA；VIRT_TO_PHYS 基于运行期
 *   g_virt_base——二者同源，影子映射的 VA/PA 与内核视图严格一致。 */
static void kpti_shadow_populate(uint64_t shadow_pml4)
{
    shadow_map_range(shadow_pml4, (uint64_t)__text_start,
                     (uint64_t)__text_end, 0);                 /* RX（只读） */
    shadow_map_range(shadow_pml4, (uint64_t)__rodata_start,
                     (uint64_t)__rodata_end, PTE_NX);          /* RO+NX */
    shadow_map_range(shadow_pml4, (uint64_t)__data_start,
                     (uint64_t)__kernel_end, PTE_WRITE | PTE_NX);
    for (int idx = 0; idx < 2; idx++) {
        shadow_map_range(shadow_pml4, IST_VA(idx, 0),
                         IST_VA(idx, IST_STACK_PAGES),
                         PTE_WRITE | PTE_NX);
    }
}

void vmm_kpti_map_kstack(uint64_t pml4_phys, uint64_t kstack_base,
                         uint64_t kstack_top)
{
    if (!g_kpti_enabled || !pml4_phys || pml4_phys == g_kernel_pml4) {
        return;
    }
    /* 内核栈来自 kmalloc（内核堆 VA），可能不页对齐：映射所有与栈区间相交
     * 的页。相邻堆对象随之进入影子属已知取舍（KAISER 同样映射内核栈）；
     * 物理直映区/其余堆页仍不可达。 */
    shadow_map_range(pml4_phys + PAGE_SIZE, kstack_base, kstack_top,
                     PTE_WRITE | PTE_NX);
}

uint64_t vmm_create_address_space(void)
{
    /* KPTI 启用：分配 8KB 对齐的物理连续对（偶页=内核视图，奇页=影子）。
     * 未启用：单页 PML4（与旧行为完全一致）。 */
    uint64_t new_pml4;
    if (g_kpti_enabled) {
        void *pair = pmm_alloc_pages_aligned(2, 2);
        if (!pair) {
            kprintf("[vmm] create_as: pmm_alloc_pages_aligned(2,2) FAILED\n");
            return 0;
        }
        new_pml4 = (uint64_t)pair;
    } else {
        void *pml4_page = pmm_alloc_page();
        if (!pml4_page) {
            return 0;
        }
        new_pml4 = (uint64_t)pml4_page;
    }

    uint64_t *dst = table_at(new_pml4);
    uint64_t *src = table_at(g_kernel_pml4);

    /* 内核视图：用户半区(0..255)清零，内核半区(256..511)共享内核映射 */
    for (int i = 0; i < 256; i++) {
        dst[i] = 0;
    }
    for (int i = 256; i < 512; i++) {
        dst[i] = src[i];
    }

    if (g_kpti_enabled) {
        /* 影子视图：pmm_alloc_pages_aligned 已整体清零（用户半区空、内核
         * 半区空），此处只需填充最小内核窗口。用户半区 PML4 项由
         * vmm_map_page 建立下级表时同步（见 kpti_sync_user_slot）。 */
        kpti_shadow_populate(new_pml4 + PAGE_SIZE);
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
            /* P0-8 KASLR：高半区基址已随机滑动 slide_v，内核半区窗口的 PDP
             * 索引须基于“随机后的虚拟地址”计算，而非固定 gb>>30。
             * vbase = KERNEL_BASE + slide_v（PHYS_TO_VIRT(0) 在 boot 期已被
             * 重定位为随机基址），phys gb 在该窗口中的 PDP 索引即：
             *   ((vbase + gb) >> 30) & 0x1FF = slide_idx + (gb>>30)。 */
            uint64_t vbase = (uint64_t)PHYS_TO_VIRT(0);
            size_t p3i = ((vbase + gb) >> 30) & 0x1FF;
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
                if (p2[i2] & PTE_HUGE) {
                    /* M4：2MB 大页（内核映射）直接释放其引用 */
                    pmm_decref((void *)(p2[i2] & PTE_ADDR_MASK));
                    p2[i2] = 0;
                    continue;
                }
                uint64_t *pt = table_at(p2[i2] & PTE_ADDR_MASK);
                for (int i1 = 0; i1 < 512; i1++) {
                    if (pt[i1] & PTE_PRESENT) {
                        /* M4 修复：用 pmm_decref 替代 pmm_free_page。
                         * 普通用户页 ref==1 时 decref 即释放；OOL 共享页
                         * (ref>1) 仅释放本地址空间的引用，避免误释放他任务
                         * 仍持有的共享物理页（此前 vmm_destroy 用
                         * pmm_free_page 无条件释放，曾可令映射到同一物理页
                         * 的其它任务页表指向已释放内存）。物理页引用释放已
                         * 统一移交给此处，port_reap_ool 不再重复 decref。 */
                        pmm_decref((void *)(pt[i1] & PTE_ADDR_MASK));
                        pt[i1] = 0;
                    }
                }
                pmm_decref((void *)(p2[i2] & PTE_ADDR_MASK));
            }
            pmm_decref((void *)(p3[i3] & PTE_ADDR_MASK));
        }
        pmm_decref((void *)(pml4[i4] & PTE_ADDR_MASK));
    }
    pmm_decref((void *)(pml4_phys & PTE_ADDR_MASK));

    /* KPTI：影子 PML4（pair+4K）仅持有「最小内核窗口」的下级页表（PDPT/PD/
     * PT，叶指向内核映像与栈的物理页——不可释放），与内核视图不共享任何
     * 下级表。逐层走查影子内核半区(256..511) 释放这些表页；用户半区(0..255)
     * 只是内核视图 PML4 项的镜像，下级表已被上方释放，此处跳过避免双重释放。
     * 最后释放影子 PML4 自身（与内核视图 PML4 各自独立 decref）。 */
    if (g_kpti_enabled && pml4_phys != g_kernel_pml4) {
        uint64_t sp = pml4_phys + PAGE_SIZE;
        uint64_t *sp4 = table_at(sp);
        for (int i4 = 256; i4 < 512; i4++) {
            if (!(sp4[i4] & PTE_PRESENT)) continue;
            uint64_t *s3 = table_at(sp4[i4] & PTE_ADDR_MASK);
            for (int i3 = 0; i3 < 512; i3++) {
                if (!(s3[i3] & PTE_PRESENT) || (s3[i3] & PTE_HUGE)) continue;
                uint64_t *s2 = table_at(s3[i3] & PTE_ADDR_MASK);
                for (int i2 = 0; i2 < 512; i2++) {
                    if (!(s2[i2] & PTE_PRESENT) || (s2[i2] & PTE_HUGE)) continue;
                    /* PT 内叶项指向内核映像/IST/内核栈物理页，属内核所有，
                     * 不释放；只回收 PT 表页本身 */
                    pmm_decref((void *)(s2[i2] & PTE_ADDR_MASK));
                }
                pmm_decref((void *)(s3[i3] & PTE_ADDR_MASK));
            }
            pmm_decref((void *)(sp4[i4] & PTE_ADDR_MASK));
        }
        pmm_decref((void *)(sp & PTE_ADDR_MASK));
    }
}

void vmm_init(void)
{
    g_kernel_pml4 = read_cr3() & PTE_ADDR_MASK;
    /* KPTI 不变量防回归断言：内核 PML4 物理地址 bit12 必须为 0。
     * KPTI 用 CR3 bit12 标记影子视图（成对 8KB 分配：偶页=内核视图，奇页=
     * 影子）；boot.S 已将 p4_table 按 8KB 对齐。若此断言失败（如未来更换
     * 引导页表来源），中断入口会把内核 CR3 误判为影子并清 bit12，引发取指
     * #PF(RSVD) 三重故障——宁可启动期 panic 也不可带病运行。 */
    if (g_kernel_pml4 & (1UL << 12)) {
        panic("vmm_init: kernel PML4 %p has bit12=1 (breaks KPTI CR3 tagging; "
              "p4_table must be 8KB-aligned)", (void *)g_kernel_pml4);
    }
    /* 扩展内核映射以覆盖全部 RAM（>4GB 支持，B4） */
    vmm_extend_kernel_mapping(pmm_total_pages() * PAGE_SIZE);
    kprintf("[vmm] kernel PML4 @ phys %p (4-level paging active)\n",
            (void *)g_kernel_pml4);
}

/* ========================================================================= */
/*  P0-5：写时复制（COW）                                                     */
/* ========================================================================= */

/* 取叶级 PTE 槽位指针（不存在返回 NULL；不建表）。COW 只作用于 4KB 叶页。 */
static uint64_t *pte_slot(uint64_t pml4_phys, uint64_t virt)
{
    size_t i4, i3, i2, i1;
    split_indices(virt, &i4, &i3, &i2, &i1);
    uint64_t *pml4 = table_at(pml4_phys);
    if (!(pml4[i4] & PTE_PRESENT)) return NULL;
    uint64_t *p3 = table_at(pml4[i4] & PTE_ADDR_MASK);
    if (!(p3[i3] & PTE_PRESENT) || (p3[i3] & PTE_HUGE)) return NULL;
    uint64_t *p2 = table_at(p3[i3] & PTE_ADDR_MASK);
    if (!(p2[i2] & PTE_PRESENT) || (p2[i2] & PTE_HUGE)) return NULL;
    uint64_t *pt = table_at(p2[i2] & PTE_ADDR_MASK);
    return &pt[i1];
}

bool vmm_fork_cow(uint64_t src_pml4, uint64_t dst_pml4)
{
    uint64_t *s4 = table_at(src_pml4);
    for (size_t i4 = 0; i4 < 256; i4++) {              /* 仅用户半区 */
        if (!(s4[i4] & PTE_PRESENT)) continue;
        uint64_t *s3 = table_at(s4[i4] & PTE_ADDR_MASK);
        for (size_t i3 = 0; i3 < 512; i3++) {
            if (!(s3[i3] & PTE_PRESENT) || (s3[i3] & PTE_HUGE)) continue;
            uint64_t *s2 = table_at(s3[i3] & PTE_ADDR_MASK);
            for (size_t i2 = 0; i2 < 512; i2++) {
                if (!(s2[i2] & PTE_PRESENT) || (s2[i2] & PTE_HUGE)) continue;
                uint64_t *st = table_at(s2[i2] & PTE_ADDR_MASK);
                for (size_t i1 = 0; i1 < 512; i1++) {
                    uint64_t pte = st[i1];
                    if (!(pte & PTE_PRESENT)) continue;
                    if (pte & PTE_OOL) continue;       /* OOL 窗口不继承 */

                    uint64_t va = (i4 << 39) | (i3 << 30)
                                | (i2 << 21) | (i1 << 12);
                    uint64_t phys = pte & PTE_ADDR_MASK;

                    uint64_t shared = pte;
                    if (pte & PTE_WRITE) {
                        /* 可写页 -> 双方降为只读 + COW 标记 */
                        shared = (pte & ~PTE_WRITE) | PTE_COW;
                        st[i1] = shared;
                        invlpg(va);                    /* src 正在使用，需刷 */
                    }
                    /* dst 建同样的（只读共享）映射；vmm_map_page 会自动
                     * 建中间页表。注意 flags 要剥掉地址位。 */
                    if (!vmm_map_page(dst_pml4, va, phys,
                                      shared & ~PTE_ADDR_MASK)) {
                        return false;
                    }
                    pmm_incref((void *)phys);          /* 双空间共享 +1 */
                }
            }
        }
    }
    return true;
}

bool vmm_cow_break(uint64_t pml4_phys, uint64_t virt)
{
    uint64_t va = virt & ~((uint64_t)PAGE_SIZE - 1);
    uint64_t *slot = pte_slot(pml4_phys, va);
    if (!slot || !(*slot & PTE_PRESENT) || !(*slot & PTE_COW)) {
        return false;
    }
    uint64_t pte  = *slot;
    uint64_t phys = pte & PTE_ADDR_MASK;

    if (pmm_refcount((void *)phys) > 1) {
        /* 仍被他空间共享：拷贝到新页，本空间独占可写 */
        void *np = pmm_alloc_page();
        if (!np) {
            return false;
        }
        memcpy(PHYS_TO_VIRT(np), PHYS_TO_VIRT(phys), PAGE_SIZE);
        *slot = ((uint64_t)np & PTE_ADDR_MASK)
              | ((pte & ~(PTE_ADDR_MASK | PTE_COW)) | PTE_WRITE);
        invlpg(va);
        pmm_decref((void *)phys);      /* 释放本空间对旧页的引用 */
    } else {
        /* 最后持有者：直接改回可写（零拷贝快路径） */
        *slot = (pte & ~PTE_COW) | PTE_WRITE;
        invlpg(va);
    }
    return true;
}
