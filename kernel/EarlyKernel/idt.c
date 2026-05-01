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

/* CPU 异常处理函数（由 idt_stubs.asm 中的 ISR 存根调用） */
void exception_handler(struct interrupt_frame *frame)
{
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
    tty_print("  RSP: ");
    tty_print_hex64((uint64_t)&frame->rax);  /* 栈顶即 RSP */
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
     * Double Fault (8) 使用 IST=1（专用栈，避免栈溢出导致的 triple fault） */
    for (int i = 0; i < 32; i++) {
        uint8_t ist = (i == 8) ? 1 : 0;   /* 仅 Double Fault 使用 IST1 */
        idt_set_gate(i, isr_table[i], GDT_SELECTOR_KERNEL_CS, IDT_GATE_INTERRUPT, ist);
    }

    load_idt();

    /* 注册伪中断向量（0xFF）- APIC 启用后可能触发
     * 参考：OSDev APIC - Spurious Interrupt Vector Register */
    idt_set_gate(APIC_SPURIOUS_VECTOR, (uint64_t)&isr_spurious,
                 GDT_SELECTOR_KERNEL_CS, IDT_GATE_INTERRUPT, 0);

    /* 对未覆盖的异常向量（32-254），CPU 访问时会触发 #GP，
     * 我们已经设置了 #GP 处理函数，可以显示调试信息 */
}
