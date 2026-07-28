/*
 * kernel/arch/x86_64/security.c
 * -----------------------------------------------------------------------------
 * P0-8 内核安全地基总装（见 include/kernel/security.h 的覆盖项说明）：
 *
 *   1) UMIP 使能（CR4.UMIP, bit 11）：禁止 Ring3 执行 sgdt/sidt/sldt/smsw/str，
 *      堵住用户态读取 GDT/IDT 线性地址（内核布局信息泄露）的经典通道；
 *   2) NXE 校验（IA32_EFER.NXE, bit 11）：确认 boot.S 已开启 NX，PTE_NX
 *      (bit 63) 才真正生效——否则用户栈/堆"不可执行"只是摆设；
 *   3) SMEP/SMAP 状态复核（CR4 bit20/21）：boot.S 依 CPUID 探测启用，
 *      此处读回 CR4 做启动期最终确认并纳入汇总报告；
 *   4) 守卫页 IST 栈：为 #DF(IST1) 与 NMI(IST2) 各建一条「底部守卫页
 *      （刻意不映射）+ 4 页映射栈」的专用栈，替换 gdt.c 的启动期静态
 *      数组栈。异常栈自身溢出时撞上未映射守卫页 -> 立刻 #PF/#DF 定格
 *      现场，而不是静默踩坏相邻内核数据（静态数组栈无此保护）；
 *   5) Meltdown 免疫检测：CPUID.(7,0):EDX[29] 表明 IA32_ARCH_CAPABILITIES
 *      MSR 存在，其 bit0 (RDCL_NO)=1 表示硬件免疫 Meltdown（Rogue Data
 *      Cache Load）。免疫 CPU 上部署 KPTI 双页表是纯性能损耗，故 SukiOS
 *      策略：检测并如实报告，仅在「不免疫」时标记 KPTI 为待部署项。
 *
 * 虚拟地址布局（新增窗口，避开既有区域）：
 *   IST 守卫栈窗口  0xFFFFD00000000000 起（独立 PML4 槽位，与
 *   KHEAP_BASE=0xFFFFC000...、KERNEL_BASE=0xFFFF8000... 物理直映区互不相交）。
 *   每条栈占 5 页 VA：页 0 = 守卫页（不映射），页 1..4 = 16KB 映射栈。
 *   两条栈窗口之间再空 1 页，防止一条栈顶溢出撞进另一条栈底。
 *
 * 调用关系：kmain() -> security_init()（vmm/kheap 就绪、smp_init 之前）。
 *   注意：本函数只替换 BSP 的 IST 栈；AP 的 TSS 由 gdt_init_ap 配静态栈
 *   （AP 不跑 Ring3、异常面小，P1 阶段再统一升级为守卫页栈）。
 */
#include <kernel/security.h>
#include <kernel/gdt.h>
#include <kernel/console.h>
#include <kernel/kaslr.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

/* boot.S 定义的全局：探测到 SMAP 后置 1，copy_*_user 据此执行 STAC/CLAC */
extern uint8_t g_smap_enabled;

/* ---- 布局常量（IST_* 系列已上移至 security.h，供 vmm.c KPTI 影子映射）---- */
#define PAGE_SIZE_4K     4096UL

#define MSR_IA32_EFER              0xC0000080U
#define MSR_IA32_ARCH_CAPABILITIES 0x0000010AU

#define CR4_UMIP  (1UL << 11)
#define CR4_SMEP  (1UL << 20)
#define CR4_SMAP  (1UL << 21)

/* ---- 特权指令隔离封装（手册 9.2：独立 noinline 函数 + 约束注释）---- */

/*
 * cpuid_ex: 执行 CPUID 指令（带子叶 ECX）。
 * 输入：leaf -> %eax, subleaf -> %ecx。
 * 输出：a/b/c/d <- eax/ebx/ecx/edx（四个输出即全部被改写的寄存器）。
 * Clobber：无额外寄存器。
 */
static __attribute__((noinline)) void cpuid_ex(uint32_t leaf, uint32_t subleaf,
                                               uint32_t *a, uint32_t *b,
                                               uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(subleaf));
}

/*
 * rdmsr64: 读取 MSR。
 * 输入：msr 号 -> %ecx。输出：edx:eax 拼接为 64 位返回值。
 * Clobber：无（输出约束已覆盖 eax/edx）。
 * 注意：读取不存在的 MSR 会 #GP，调用方必须先经 CPUID 确认 MSR 存在。
 */
static __attribute__((noinline)) uint64_t rdmsr64(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/*
 * read_cr4 / write_cr4: 读/写控制寄存器 CR4。
 * write_cr4 会立即改变 CPU 全局行为（UMIP 等），Clobber 加 memory 屏障
 * 防止编译器跨界重排内存访问。
 */
static __attribute__((noinline)) uint64_t read_cr4(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

static __attribute__((noinline)) void write_cr4(uint64_t v)
{
    __asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory");
}

/* ---- 守卫页 IST 栈 ----
 * 窗口布局（idx = 0 -> IST1/#DF，idx = 1 -> IST2/NMI）：
 *   base = IST_GUARD_BASE + idx * IST_WINDOW_PAGES * 4K
 *   [base + 0*4K]           守卫页：刻意不映射（溢出即 #PF）
 *   [base + 1*4K .. 4*4K]   4 页映射栈（PTE_NX：栈永不需要执行权限）
 *   栈顶 = base + (1 + IST_STACK_PAGES) * 4K（16 字节对齐天然满足）
 * 返回栈顶 VA；任一页分配/映射失败返回 0（调用方保留原静态栈，不降级安全性）。 */
static uint64_t ist_guard_stack_create(int idx)
{
    uint64_t base = IST_GUARD_BASE +
                    (uint64_t)idx * IST_WINDOW_PAGES * PAGE_SIZE_4K;
    uint64_t pml4 = vmm_kernel_pml4();

    for (int i = 0; i < IST_STACK_PAGES; i++) {
        void *phys = pmm_alloc_page();
        if (!phys) {
            return 0;
        }
        uint64_t va = base + (uint64_t)(1 + i) * PAGE_SIZE_4K;
        if (!vmm_map_page(pml4, va, (uint64_t)phys,
                          PTE_PRESENT | PTE_WRITE | PTE_NX)) {
            pmm_free_page(phys);
            return 0;
        }
    }
    return base + (uint64_t)(1 + IST_STACK_PAGES) * PAGE_SIZE_4K;
}

/* 在当前 CPU 上探测并应用安全控制位（SMEP/SMAP/UMIP/EFER.NXE）。
 * 设计为幂等：BSP 在 security_init 调用，AP 在 ap_main 调用，二者走同一路径，
 * 保证每核的用户内存保护位一致（AP 漏设 SMEP/SMAP 会让用户态真实可执行内核
 * 页 / 访问内核数据，是真实安全缺口）。 */
void cpu_apply_security_features(void)
{
    uint32_t a, b, c, d;
    cpuid_ex(7, 0, &a, &b, &c, &d);

    uint64_t cr4 = read_cr4();
    if (c & (1U << 2))  { cr4 |= CR4_UMIP; }   /* UMIP */
    if (b & (1U << 7))  { cr4 |= CR4_SMEP; }   /* SMEP */
    if (b & (1U << 19)) { cr4 |= CR4_SMAP;     /* SMAP */
                            g_smap_enabled = 1; }
    write_cr4(cr4);

    /* EFER.NXE：CPUID.80000001H:EDX[20] 探测 */
    if (!(rdmsr64(MSR_IA32_EFER) & (1UL << 11))) {
        cpuid_ex(0x80000000U, 0, &a, &b, &c, &d);
        if (a >= 0x80000001U) {
            cpuid_ex(0x80000001U, 0, &a, &b, &c, &d);
            if (d & (1U << 20)) {
                uint64_t efer = rdmsr64(MSR_IA32_EFER);
                efer |= (1UL << 11);
                /* 写 EFER 用 rdmsr64/wrmsr 封装不便，单独内联 */
                uint32_t lo = (uint32_t)efer, hi = (uint32_t)(efer >> 32);
                __asm__ volatile("wrmsr" : : "c"(MSR_IA32_EFER), "a"(lo), "d"(hi) : "memory");
            }
        }
    }
}

void security_init(void)
{
    uint32_t a, b, c, d;

    /* ---- 1) 在当前 BSP 上应用安全控制位（UMIP/SMEP/SMAP/NXE） ---- */
    cpu_apply_security_features();

    cpuid_ex(7, 0, &a, &b, &c, &d);
    bool has_umip = (c & (1U << 2)) != 0;
    bool has_arch_cap = (d & (1U << 29)) != 0;   /* EDX[29]: ARCH_CAPABILITIES */
    bool umip_on = (read_cr4() & CR4_UMIP) != 0;

    /* ---- 2) NXE 校验（boot.S/ap_boot.S 应已置位；此处只验证） ---- */
    bool nxe_on = (rdmsr64(MSR_IA32_EFER) & (1UL << 11)) != 0;

    /* ---- 3) SMEP/SMAP 读回复核 ---- */
    uint64_t cr4 = read_cr4();
    bool smep_on = (cr4 & CR4_SMEP) != 0;
    bool smap_on = (cr4 & CR4_SMAP) != 0;

    /* ---- 4) 守卫页 IST 栈（BSP）：IST1=#DF, IST2=NMI ---- */
    uint64_t df_top  = ist_guard_stack_create(0);
    uint64_t nmi_top = ist_guard_stack_create(1);
    if (df_top)  { tss_set_ist(1, df_top);  }
    if (nmi_top) { tss_set_ist(2, nmi_top); }

    /* ---- 5) Meltdown 免疫检测 -> KPTI 部署（P0-R2） ----
     * RDCL_NO=1（硬件免疫）：双页表纯性能损耗，不部署；
     * 其余（明确易损 / 无 ARCH_CAPABILITIES 无从判断）：启用 KPTI。
     * 置位必须在本函数内完成——kmain 顺序保证 security_init 先于
     * sched_init/task_create_user，任何会进入 Ring3 的地址空间都将以
     * 「内核视图+影子」成对创建（vmm_create_address_space）。 */
    bool rdcl_no = false;
    if (has_arch_cap) {
        rdcl_no = (rdmsr64(MSR_IA32_ARCH_CAPABILITIES) & 1UL) != 0;
    }
    if (!rdcl_no) {
        g_kpti_enabled = 1;
    }

    /* ---- 汇总报告 ---- */
    kprintf("[security] SMEP=%s SMAP=%s UMIP=%s NXE=%s\n",
            smep_on ? "on" : "OFF",
            smap_on ? "on" : "OFF",
            umip_on ? "on" : (has_umip ? "FAIL" : "n/a"),
            nxe_on  ? "on" : "OFF");
    kprintf("[security] IST guard stacks: #DF %s @%p, NMI %s @%p (16KB+guard)\n",
            df_top  ? "installed" : "FALLBACK-static", (void *)df_top,
            nmi_top ? "installed" : "FALLBACK-static", (void *)nmi_top);
    kprintf("[security] stack canary reseeded (TSC entropy, NUL byte); "
            "heap base slide active\n");
    kprintf("[security] Meltdown: %s -> KPTI %s\n",
            rdcl_no ? "immune (RDCL_NO=1)"
                    : (has_arch_cap ? "VULNERABLE (RDCL_NO=0)"
                                    : "unknown (no ARCH_CAPABILITIES)"),
            g_kpti_enabled
                ? "ACTIVE (KAISER dual page tables: shadow PML4 maps only "
                  "kernel image + IST + task kstacks)"
                : "not needed");

    /* ---- 6) KASLR（P0-8 代码段随机化）状态报告 ----
     * g_kernel_slide 由 boot.S 在引导期写入（高半区基址随机滑动量）。
     * 0 表示退化（未随机化）；非 0 表示内核虚拟基址已偏离固定 KERNEL_BASE。 */
    if (g_kernel_slide == 0) {
        kprintf("[security] KASLR: DISABLED (slide=0, base fixed)\n");
    } else {
        kprintf("[security] KASLR: ENABLED slide=%lu MiB (base=0xFFFF800000000000+slide)\n",
                (unsigned long)(g_kernel_slide / (1024 * 1024)));
    }
}
