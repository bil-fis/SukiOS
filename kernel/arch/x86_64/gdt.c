/*
 * kernel/arch/x86_64/gdt.c
 * -----------------------------------------------------------------------------
 * 构建并加载 GDT 与 TSS。
 *
 * 调用关系：kmain() -> gdt_init()。之后 IDT/中断/调度器方可安全运行；
 *           进入 Ring3 前须先由 tss_set_rsp0() 设定内核栈。
 */
#include <kernel/gdt.h>
#include <kernel/percpu.h>
#include <kernel/console.h>

/* 64 位 TSS 结构 */
struct tss_entry {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

/* GDT：5 个 8 字节描述符 + 1 个 16 字节 TSS 描述符 = 7 个 8 字节槽 */
static uint64_t g_gdt[7];
static struct tss_entry g_tss;

/* GDTR */
struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct gdt_ptr g_gdtr;

/* 为 Ring3->Ring0 中断预留的独立 IST 栈（例如缺页/双重错误更安全） */
static uint8_t g_ist_stack[16384] __attribute__((aligned(16)));

/* 构造一个标准 8 字节代码/数据段描述符 */
static uint64_t make_segment(bool code, int dpl, bool long_mode)
{
    uint64_t desc = 0;
    /* access byte (bits 40-47) */
    uint64_t access = 0x92;                 /* present, S=1, data, writable */
    if (code) {
        access = 0x9A;                      /* present, S=1, code, readable */
    }
    access |= ((uint64_t)(dpl & 3)) << 5;   /* DPL */
    desc |= access << 40;
    /* flags (bits 52-55)：L 位 = 长模式代码段 */
    if (long_mode && code) {
        desc |= (uint64_t)0x2 << 52;        /* L=1 */
    } else {
        desc |= (uint64_t)0x0 << 52;
    }
    /* limit/base 在长模式下被忽略，置常见值即可 */
    return desc;
}

/* 在 g_gdt[idx]/[idx+1] 写入 16 字节 TSS 描述符 */
static void set_tss_descriptor(int idx, uint64_t base, uint32_t limit)
{
    uint64_t low = 0;
    low |= (limit & 0xFFFF);
    low |= (base & 0xFFFFFF) << 16;
    low |= (uint64_t)0x89 << 40;             /* present, type=0x9 (64-bit TSS available) */
    low |= (uint64_t)((limit >> 16) & 0xF) << 48;
    low |= ((base >> 24) & 0xFF) << 56;
    g_gdt[idx] = low;
    g_gdt[idx + 1] = (base >> 32) & 0xFFFFFFFF;   /* 高 32 位 base */
}

/* ---- 特权指令隔离封装（手册 9.2）---- */

/*
 * gdt_load: 执行 lgdt 加载 GDTR，随后通过远返回重载 CS，并重载数据段寄存器。
 * 输入：ptr = 指向 struct gdt_ptr 的指针（%rdi）。
 * Clobber：rax（用于段值与返回地址），内存屏障。
 * 说明：lretq 用 0x08 作为新 CS 完成 CS 重载（长模式无法用 mov 改 CS）。
 */
static __attribute__((noinline)) void gdt_load(const struct gdt_ptr *ptr)
{
    __asm__ volatile(
        "lgdt (%0)\n\t"
        "pushq $0x08\n\t"            /* 新 CS = 内核代码段 */
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"                    /* 远返回，重载 CS */
        "1:\n\t"
        "movw $0x10, %%ax\n\t"       /* 数据段 = 内核数据段 */
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        :
        : "r"(ptr)
        : "rax", "memory");
}

/*
 * tss_load: 执行 ltr 加载任务寄存器 (TR)。
 * 输入：sel = TSS 选择子（%0）。
 * Clobber：无显式寄存器，加内存屏障。
 */
static __attribute__((noinline)) void tss_load(uint16_t sel)
{
    __asm__ volatile("ltr %0" : : "r"(sel) : "memory");
}

void tss_set_rsp0(uint64_t rsp0)
{
    g_tss.rsp0 = rsp0;
}

void gdt_init(void)
{
    /* 初始化 TSS */
    for (size_t i = 0; i < sizeof(g_tss); i++) {
        ((uint8_t *)&g_tss)[i] = 0;
    }
    g_tss.rsp0 = (uint64_t)0;                       /* 稍后由调度器/进入用户态前设置 */
    g_tss.ist[0] = (uint64_t)(g_ist_stack + sizeof(g_ist_stack)); /* IST1 */
    g_tss.iomap_base = sizeof(struct tss_entry);    /* 无 I/O 位图 */

    /* 段描述符 */
    g_gdt[0] = 0;                                     /* 0x00 null */
    g_gdt[1] = make_segment(true,  0, true);         /* 0x08 内核代码 */
    g_gdt[2] = make_segment(false, 0, false);        /* 0x10 内核数据 */
    g_gdt[3] = make_segment(true,  3, true);         /* 0x18 用户代码 */
    g_gdt[4] = make_segment(false, 3, false);        /* 0x20 用户数据 */
    set_tss_descriptor(5, (uint64_t)&g_tss, sizeof(struct tss_entry) - 1); /* 0x28 TSS */

    g_gdtr.limit = sizeof(g_gdt) - 1;
    g_gdtr.base  = (uint64_t)&g_gdt;

    gdt_load(&g_gdtr);
    tss_load(GDT_TSS_SELECTOR);

    kprintf("[gdt] GDT+TSS loaded (kcode=0x08 kdata=0x10 ucode=0x1B udata=0x23 tss=0x28)\n");
}

/* ---- P0-3：AP 专属 GDT/TSS ----
 * TR（任务寄存器）指向的 TSS busy 位是每 CPU 的：多个 CPU 不能共享同一
 * TSS 描述符（第二次 ltr 会因 busy 位置位而 #GP）。故每个 AP 一套
 * GDT + TSS + IST 栈。AP 当前不跑 Ring3 任务，TSS.rsp0 暂不使用，但
 * IST1 必须有效（IDT 向量 8 双重错误配置了 IST1）。 */
static uint64_t g_gdt_ap[MAX_CPUS][7];
static struct tss_entry g_tss_ap[MAX_CPUS];
static uint8_t g_ist_ap[MAX_CPUS][4096] __attribute__((aligned(16)));
static struct gdt_ptr g_gdtr_ap[MAX_CPUS];

void gdt_init_ap(uint32_t cpu)
{
    if (cpu >= MAX_CPUS) {
        return;
    }
    struct tss_entry *tss = &g_tss_ap[cpu];
    for (size_t i = 0; i < sizeof(*tss); i++) {
        ((uint8_t *)tss)[i] = 0;
    }
    tss->ist[0] = (uint64_t)(g_ist_ap[cpu] + sizeof(g_ist_ap[cpu]));
    tss->iomap_base = sizeof(struct tss_entry);

    uint64_t *gdt = g_gdt_ap[cpu];
    gdt[0] = 0;
    gdt[1] = make_segment(true,  0, true);
    gdt[2] = make_segment(false, 0, false);
    gdt[3] = make_segment(true,  3, true);
    gdt[4] = make_segment(false, 3, false);
    /* TSS 描述符（16 字节，占槽 5/6），指向本 CPU 的 TSS */
    {
        uint64_t base = (uint64_t)tss;
        uint32_t limit = sizeof(struct tss_entry) - 1;
        uint64_t low = 0;
        low |= (limit & 0xFFFF);
        low |= (base & 0xFFFFFF) << 16;
        low |= (uint64_t)0x89 << 40;
        low |= (uint64_t)((limit >> 16) & 0xF) << 48;
        low |= ((base >> 24) & 0xFF) << 56;
        gdt[5] = low;
        gdt[6] = (base >> 32) & 0xFFFFFFFF;
    }

    g_gdtr_ap[cpu].limit = sizeof(g_gdt_ap[cpu]) - 1;
    g_gdtr_ap[cpu].base  = (uint64_t)gdt;
    gdt_load(&g_gdtr_ap[cpu]);
    tss_load(GDT_TSS_SELECTOR);
}
