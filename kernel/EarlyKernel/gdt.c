/* =============================================================================
 * SukiOS - 64 位 GDT 初始化
 *
 * 参考：OSDev GDT Tutorial, AMD64 APM Vol.2 4.8.1-4.8.2
 *
 * 在 long mode 中，段基地址和段界限被忽略（平坦模型），
 * 但 TSS 仍然需要正确设置。
 * ============================================================================= */

#include "kernel/gdt.h"
#include "kernel/tty.h"
#include "kernel/io.h"

/* GDT 表（7 个 64 位条目） */
static uint64_t gdt_entries[GDT_ENTRIES];

/* TSS 实例（BSS 段，链接器初始化为零） */
struct tss_entry kernel_tss;

/* ISR 存根中的栈符号（定义在 idt_stubs.asm 的 .bss 段） */
extern uint8_t double_fault_stack_top;
extern uint8_t nmi_stack_top;
extern uint8_t machine_check_stack_top;
extern uint8_t kernel_interrupt_stack_top;

/* ---- GDT 编码辅助函数 ---- */

/* 编码普通段描述符（代码段/数据段）
 * 参考：OSDev GDT Tutorial - Flat / Long Mode Setup */
static inline uint64_t make_seg(uint32_t limit, uint32_t base,
                                uint8_t access, uint8_t flags)
{
    uint64_t desc = 0;
    desc |= (limit & 0xFFFF);                        /* Limit bits 0-15 */
    desc |= ((uint64_t)(base & 0xFFFF)) << 16;      /* Base bits 0-15 */
    desc |= ((uint64_t)(base >> 16) & 0xFF) << 32;  /* Base bits 16-23 */
    desc |= ((uint64_t)access) << 40;                /* Access byte */
    desc |= ((uint64_t)(limit >> 16) & 0x0F) << 48; /* Limit bits 16-19 (byte 6 低 4 位) */
    desc |= ((uint64_t)(flags & 0x0F)) << 52;       /* Flags (byte 6 高 4 位: G,D/B,L,AVL) */
    desc |= ((uint64_t)(base >> 24) & 0xFF) << 56;   /* Base bits 24-31 */
    return desc;
}

/* 编码 64 位 TSS 系统段描述符（16 字节 = 2 个 GDT 条目）
 * 参考：OSDev GDT - Long Mode System Segment Descriptor
 *       AMD64 APM Vol.2 Figure 4-15 */
static void make_tss_desc(uint64_t *low, uint64_t *high,
                          uint64_t base, uint32_t limit)
{
    /* 低 8 字节 */
    *low = 0;
    *low |= (limit & 0xFFFF);                        /* Limit bits 0-15 */
    *low |= ((uint64_t)(base & 0xFFFF)) << 16;      /* Base bits 0-15 */
    *low |= ((uint64_t)(base >> 16) & 0xFF) << 32;  /* Base bits 16-23 */
    *low |= ((uint64_t)0x89) << 40;                   /* Access: P=1,DPL=0,S=0,Type=0b1001 */
    *low |= ((uint64_t)(limit >> 16) & 0x0F) << 48; /* Limit bits 16-19 */
    *low |= ((uint64_t)(base >> 24) & 0xFF) << 56;   /* Base bits 24-31 */

    /* 高 8 字节 */
    *high = (base >> 32) & 0xFFFFFFFF;               /* Base bits 32-63 */
}

/* 加载 GDT 并刷新段寄存器
 * 参考：OSDev GDT Tutorial - Long Mode reloadSegments */
static void load_gdt(uint64_t *gdt_base, uint16_t gdt_limit)
{
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) gdtr;
    gdtr.limit = gdt_limit;
    gdtr.base = (uint64_t)gdt_base;

    __asm__ volatile (
        "lgdt %[gdtr]\n\t"
        /* 通过 far return 重新加载 CS（long mode 不能用 far jump） */
        "pushq $0x08\n\t"                  /* 内核代码段选择子 */
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        /* 重新加载数据段寄存器 */
        "movw $0x10, %%ax\n\t"             /* 内核数据段选择子 */
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        :
        : [gdtr] "m"(gdtr)
        : "rax", "memory"
    );
}

void gdt_init(void)
{
    /* 清零 GDT */
    for (int i = 0; i < GDT_ENTRIES; i++)
        gdt_entries[i] = 0;

    /* [0x00] 空描述符 */
    gdt_entries[0] = 0;

    /* [0x08] 内核 64 位代码段
     * Access = 0x9A: P=1, DPL=0, S=1, Type=0xA (Execute/Read)
     * Flags  = 0xA:  L=1, DB=0 (long mode) */
    gdt_entries[1] = make_seg(0xFFFFF, 0, 0x9A, 0xA);

    /* [0x10] 内核数据段
     * Access = 0x92: P=1, DPL=0, S=1, Type=0x2 (Read/Write)
     * Flags  = 0xC:  G=1, D/B=1 */
    gdt_entries[2] = make_seg(0xFFFFF, 0, 0x92, 0xC);

    /* [0x18] 用户 64 位代码段
     * Access = 0xFA: P=1, DPL=3, S=1, Type=0xA */
    gdt_entries[3] = make_seg(0xFFFFF, 0, 0xFA, 0xA);

    /* [0x20] 用户数据段
     * Access = 0xF2: P=1, DPL=3, S=1, Type=0x2 */
    gdt_entries[4] = make_seg(0xFFFFF, 0, 0xF2, 0xC);

    /* [0x28-0x30] TSS（64 位系统段，占 2 个条目）
     * 参考：AMD64 APM Vol.2 Figure 4-15, OSDev GDT - TSS Descriptor
     *
     * 设置 TSS 栈：
     *   RSP0: ring3→ring0 中断时 CPU 自动加载此栈（特权级切换必须）
     *   IST1: Double Fault 专用栈（内核栈可能已损坏）
     *   IST2: NMI 专用栈（可随时中断任意内核代码）
     *   IST3: Machine Check 专用栈（不可屏蔽 abort，可随时发生）
     * 参考：Intel SDM Vol.3 6.12.1 (RSP0), 6.14.1 (IST) */
    kernel_tss.rsp0 = (uint64_t)&kernel_interrupt_stack_top;
    kernel_tss.ist1 = (uint64_t)&double_fault_stack_top;
    kernel_tss.ist2 = (uint64_t)&nmi_stack_top;
    kernel_tss.ist3 = (uint64_t)&machine_check_stack_top;
    /* I/O 权限位图偏移 = TSS 大小，表示无位图 → ring3 访问任何 I/O 端口均触发 #GP */
    kernel_tss.iopb_offset = sizeof(struct tss_entry);

    make_tss_desc(&gdt_entries[5], &gdt_entries[6],
                  (uint64_t)&kernel_tss, sizeof(struct tss_entry) - 1);

    /* 加载 GDT 并刷新段寄存器 */
    load_gdt(gdt_entries, sizeof(gdt_entries) - 1);

    /* 加载 TSS（LTR 指令）
     * 参考：Intel SDM Vol.3 6.2.4 - Task Register */
    __asm__ volatile ("ltr %w0" : : "r"(GDT_SELECTOR_TSS));
}
