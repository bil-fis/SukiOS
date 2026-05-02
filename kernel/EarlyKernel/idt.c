/* =============================================================================
 * SukiOS - 64 位 IDT 初始化 + CPU 异常处理
 *
 * 参考：OSDev IDT, Intel SDM Vol.3 6.14
 *
 * 为 CPU 异常（0-31）设置中断门，IDT 条目为 16 字节。
 * ISR 汇编存根在 idt_stubs.asm 中定义。
 * ============================================================================= */

#include "kernel/gdt.h"
#include "kernel/idt.h"
#include "kernel/tty.h"
#include "kernel/io.h"
#include "kernel/apic.h"

static struct idt_entry idt[IDT_ENTRIES];

/* CPU 异常名称（参考 Intel SDM Table 6-1） */
static const char *exception_names[] = {
    "Division By Zero",            /* 0x00 #DE */
    "Debug",                       /* 0x01 #DB */
    "Non Maskable Interrupt",      /* 0x02 NMI */
    "Breakpoint",                  /* 0x03 #BP */
    "Overflow",                    /* 0x04 #OF */
    "Bound Range Exceeded",        /* 0x05 #BR */
    "Invalid Opcode",              /* 0x06 #UD */
    "Device Not Available",        /* 0x07 #NM */
    "Double Fault",                /* 0x08 #DF */
    "Coprocessor Segment Overrun", /* 0x09 */
    "Invalid TSS",                 /* 0x0A #TS */
    "Segment Not Present",         /* 0x0B #NP */
    "Stack-Segment Fault",         /* 0x0C #SS */
    "General Protection Fault",    /* 0x0D #GP */
    "Page Fault",                  /* 0x0E #PF */
    "Reserved",                    /* 0x0F */
    "x87 FPU Error",              /* 0x10 #MF */
    "Alignment Check",             /* 0x11 #AC */
    "Machine Check",               /* 0x12 #MC */
    "SIMD Floating-Point",         /* 0x13 #XM */
    "Virtualization Exception",    /* 0x14 #VE */
    "Control Protection",          /* 0x15 #CP */
};

/* ISR 存根地址（在 idt_stubs.asm 中定义） */
extern uint64_t isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7;
extern uint64_t isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15;
extern uint64_t isr16, isr17, isr18, isr19, isr20, isr21;
extern uint64_t isr22, isr23, isr24, isr25, isr26, isr27;
extern uint64_t isr28, isr29, isr30, isr31;

/* 伪中断 ISR 255（由 apic_init 启用 LAPIC 后可能触发） */
extern uint64_t isr_spurious;

/* IRQ 存根地址（在 idt_stubs.asm 中定义，向量 0x20-0x2F） */
extern uint64_t irq32, irq33, irq34, irq35, irq36, irq37, irq38, irq39;
extern uint64_t irq40, irq41, irq42, irq43, irq44, irq45, irq46, irq47;

/* IRQ 处理函数注册表（在 InitKernel 阶段由驱动注册） */
static irq_handler_fn irq_handlers[16] = { NULL };

static uint64_t isr_table[32] = {
    (uint64_t)&isr0,  (uint64_t)&isr1,  (uint64_t)&isr2,  (uint64_t)&isr3,
    (uint64_t)&isr4,  (uint64_t)&isr5,  (uint64_t)&isr6,  (uint64_t)&isr7,
    (uint64_t)&isr8,  (uint64_t)&isr9,  (uint64_t)&isr10, (uint64_t)&isr11,
    (uint64_t)&isr12, (uint64_t)&isr13, (uint64_t)&isr14, (uint64_t)&isr15,
    (uint64_t)&isr16, (uint64_t)&isr17, (uint64_t)&isr18, (uint64_t)&isr19,
    (uint64_t)&isr20, (uint64_t)&isr21, (uint64_t)&isr22, (uint64_t)&isr23,
    (uint64_t)&isr24, (uint64_t)&isr25, (uint64_t)&isr26, (uint64_t)&isr27,
    (uint64_t)&isr28, (uint64_t)&isr29, (uint64_t)&isr30, (uint64_t)&isr31,
};

void idt_set_gate(uint8_t n, uint64_t handler, uint16_t selector,
                  uint8_t type_attr, uint8_t ist)
{
    idt[n].offset_low  = (uint16_t)(handler & 0xFFFF);
    idt[n].offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
    idt[n].offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFF);
    idt[n].selector    = selector;
    idt[n].ist         = ist;
    idt[n].type_attr   = type_attr;
    idt[n].reserved    = 0;
}

/* APIC 伪中断处理（ISR 255）
 * APIC 启用后可能产生伪中断，只需发送 EOI 后立即返回
 * 参考：Intel SDM Vol.3 10.4.7 Spurious Interrupt */
void spurious_irq_handler(struct interrupt_frame *frame)
{
    (void)frame;

    /* 发送 LAPIC EOI（End of Interrupt）
     * 如果 apic_lapic_base 尚未初始化（InitKernel 之前），则忽略 */
    if (apic_lapic_base) {
        lapic_write(LAPIC_EOI, 0);
    }
}

/* CPU 异常处理函数（由 idt_stubs.asm 中的 ISR 存根调用）
 * 参考：Intel SDM Vol.3 Table 6-1 (exception classes) */
void exception_handler(struct interrupt_frame *frame)
{
    /* 检测特权级变化：CS 低 2 位 = CPL
     * 如果 CPL != 0，说明异常来自用户态（ring3） */
    uint8_t from_ring = frame->cs & 0x3;

    tty_setcolor(VGA_RED, VGA_BLACK);
    tty_print("\n!!! CPU Exception: ");

    if (frame->int_no < sizeof(exception_names) / sizeof(exception_names[0]))
        tty_print(exception_names[frame->int_no]);
    else
        tty_print("Unknown");

    tty_print("\n  INT: ");
    tty_print_dec((uint32_t)frame->int_no);
    tty_print("  ERR: ");
    tty_print_hex64(frame->err_code);
    tty_print("\n  RIP: ");
    tty_print_hex64(frame->rip);
    tty_print("  CS: ");
    tty_print_hex64(frame->cs);
    tty_print("  RFLAGS: ");
    tty_print_hex64(frame->rflags);
    tty_print("\n  RSP: ");
    tty_print_hex64((uint64_t)&frame->rax);  /* 当前栈顶（内核栈/RSP0） */

    /* 仅在特权级变化时打印用户态的 SS:RSP（由 CPU 压入）
     * 参考：Intel SDM Vol.3 Figure 6-8 */
    if (from_ring != 0) {
        tty_print("\n  [User] RSP: ");
        tty_print_hex64(frame->rsp);
        tty_print("  SS: ");
        tty_print_hex64(frame->ss);
    }
    tty_print("\n");

    /* 停机 */
    for (;;) __asm__ volatile ("cli; hlt");
}

/* 加载 IDT */
static void load_idt(void)
{
    struct idt_ptr idtr;
    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint64_t)idt;

    __asm__ volatile ("lidt %0" : : "m"(idtr));
}

void idt_init(void)
{
    /* 清零整个 IDT */
    for (int i = 0; i < IDT_ENTRIES; i++)
        idt[i].offset_low = idt[i].offset_mid = idt[i].offset_high = 0;

    /* 设置 CPU 异常门（0-31）
     * 参考：Intel SDM Vol.3 Table 6-1, OSDev IDT
     *
     * IST 分配策略：
     *   Double Fault (#DF, 8):  IST1 — 内核栈可能已损坏，必须独立栈
     *   NMI (2):               IST2 — 可在任何时刻中断内核（包括中断处理中）
     *   Machine Check (#MC, 18): IST3 — 不可屏蔽 abort，可随时发生
     *   其余异常:              IST0 — 使用当前栈（无特权级变化时）或 RSP0 */
    for (int i = 0; i < 32; i++) {
        uint8_t ist = 0;
        switch (i) {
        case 2:  ist = IST_NMI; break;   /* NMI */
        case 8:  ist = IST_DF;  break;   /* Double Fault */
        case 18: ist = IST_MC;  break;   /* Machine Check */
        default: break;
        }
        idt_set_gate(i, isr_table[i], GDT_SELECTOR_KERNEL_CS, IDT_GATE_INTERRUPT, ist);
    }

    load_idt();

    /* 注册伪中断向量（0xFF）- APIC 启用后可能触发
     * 参考：OSDev APIC - Spurious Interrupt Vector Register */
    idt_set_gate(APIC_SPURIOUS_VECTOR, (uint64_t)&isr_spurious,
                 GDT_SELECTOR_KERNEL_CS, IDT_GATE_INTERRUPT, 0);

    /* 设置 IRQ 门（向量 0x20-0x2F）
     * IRQ 存根在 idt_stubs.asm 中定义
     * 注意：此时 IOAPIC 尚未初始化，IRQ 不会实际触发
     * 各驱动的 IOAPIC 重定向条目在 InitKernel 阶段配置 */
    static uint64_t irq_table[16] = {
        (uint64_t)&irq32, (uint64_t)&irq33, (uint64_t)&irq34, (uint64_t)&irq35,
        (uint64_t)&irq36, (uint64_t)&irq37, (uint64_t)&irq38, (uint64_t)&irq39,
        (uint64_t)&irq40, (uint64_t)&irq41, (uint64_t)&irq42, (uint64_t)&irq43,
        (uint64_t)&irq44, (uint64_t)&irq45, (uint64_t)&irq46, (uint64_t)&irq47,
    };
    for (int i = 0; i < 16; i++) {
        idt_set_gate(0x20 + i, irq_table[i],
                     GDT_SELECTOR_KERNEL_CS, IDT_GATE_INTERRUPT, 0);
    }

    /* 对未覆盖的异常向量（32-254），CPU 访问时会触发 #GP，
     * 我们已经设置了 #GP 处理函数，可以显示调试信息 */
}

/* IRQ 处理函数注册 */
void irq_register_handler(uint8_t irq_num, irq_handler_fn handler)
{
    if (irq_num < 16) {
        irq_handlers[irq_num] = handler;
    }
}

/* IRQ 通用处理函数（由 idt_stubs.asm 的 irq_common_stub 调用）
 * 参考：Intel SDM Vol.3 6.8, OSDev IRQ Handling
 *
 * 中断处理流程：
 *   1. 保存寄存器（汇编存根完成）
 *   2. 调用已注册的 IRQ 处理函数
 *   3. 发送 LAPIC EOI（必须在中断处理完成后发送）
 *   4. 恢复寄存器并 iretq（汇编存根完成） */
void irq_handler(struct interrupt_frame *frame)
{
    /* 计算 IRQ 编号：向量号 - IRQ_BASE */
    uint8_t irq_num = (uint8_t)(frame->int_no - 0x20);

    if (irq_num < 16 && irq_handlers[irq_num]) {
        /* 调用已注册的处理函数，传入中断向量号 */
        irq_handlers[irq_num]((uint8_t)frame->int_no);
    }

    /* 发送 LAPIC EOI（End of Interrupt）
     * 必须在处理完成后发送，否则 LAPIC 不会再转发后续中断
     * 参考：Intel SDM Vol.3 10.8.5 */
    if (apic_lapic_base) {
        lapic_write(LAPIC_EOI, 0);
    }
}
