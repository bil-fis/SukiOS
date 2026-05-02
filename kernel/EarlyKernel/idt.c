/* =============================================================================
 * SukiOS - 64 位 IDT 初始化 + CPU 异常处理 + syscall
 *
 * 参考：OSDev IDT, Intel SDM Vol.3 6.14, AMD64 APM Vol.2 12.4.1
 *
 * 使用统一的 rdmsr / wrmsr，并验证 LSTAR 写入。
 * ============================================================================= */

#include "kernel/gdt.h"
#include "kernel/idt.h"
#include "kernel/tty.h"
#include "kernel/io.h"
#include "kernel/apic.h"
#include "kernel/keyboard.h"

static struct idt_entry idt[IDT_ENTRIES];

static const char *exception_names[] = {
    "Division By Zero", "Debug", "Non Maskable Interrupt", "Breakpoint",
    "Overflow", "Bound Range Exceeded", "Invalid Opcode",
    "Device Not Available", "Double Fault", "Coprocessor Segment Overrun",
    "Invalid TSS", "Segment Not Present", "Stack-Segment Fault",
    "General Protection Fault", "Page Fault", "Reserved",
    "x87 FPU Error", "Alignment Check", "Machine Check",
    "SIMD Floating-Point", "Virtualization Exception", "Control Protection",
};

extern uint64_t isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7;
extern uint64_t isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15;
extern uint64_t isr16, isr17, isr18, isr19, isr20, isr21;
extern uint64_t isr22, isr23, isr24, isr25, isr26, isr27;
extern uint64_t isr28, isr29, isr30, isr31;
extern uint64_t isr_spurious;

extern uint64_t irq32, irq33, irq34, irq35, irq36, irq37, irq38, irq39;
extern uint64_t irq40, irq41, irq42, irq43, irq44, irq45, irq46, irq47;

extern uint64_t syscall_stub;
extern uint8_t kernel_interrupt_stack_top;

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

/* ---- syscall 初始化（验证 LSTAR） ---- */
static void setup_syscall(void)
{
    uint64_t stub_addr = (uint64_t)&syscall_stub;

    tty_setcolor(VGA_CYAN, VGA_BLACK);
    tty_print("  [syscall] LSTAR target = 0x");
    tty_print_hex64(stub_addr);
    tty_print("\n");
    tty_setcolor(VGA_LIGHT_GREY, VGA_BLACK);

    wrmsr(MSR_IA32_LSTAR, stub_addr);

    /* 读回验证 */
    uint64_t lstar = rdmsr(MSR_IA32_LSTAR);
    tty_print("  [syscall] LSTAR readback = 0x");
    tty_print_hex64(lstar);
    tty_print("\n");

    if (lstar != stub_addr) {
        tty_setcolor(VGA_RED, VGA_BLACK);
        tty_print("  ERROR: LSTAR mismatch! Check MSR access.\n");
        tty_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
        for (;;) __asm__ volatile ("cli; hlt");
    }
}

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

void spurious_irq_handler(struct interrupt_frame *frame)
{
    (void)frame;
    if (apic_lapic_base) {
        lapic_write(LAPIC_EOI, 0);
    }
}

void exception_handler(struct interrupt_frame *frame)
{
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
    tty_print_hex64((uint64_t)frame->rsp);

    if (frame->int_no == 14) {
        uint64_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        tty_print("\n  CR2: ");
        tty_print_hex64(cr2);
    }

    if (from_ring != 0) {
        tty_print("\n  [User] RSP: ");
        tty_print_hex64(frame->rsp);
        tty_print("  SS: ");
        tty_print_hex64(frame->ss);
    }

    tty_print("\n  RDI=");  tty_print_hex64(frame->rdi);
    tty_print(" RSI=");   tty_print_hex64(frame->rsi);
    tty_print(" RAX=");   tty_print_hex64(frame->rax);
    tty_print(" RBP=");   tty_print_hex64(frame->rbp);
    tty_print("\n");

    for (;;) __asm__ volatile ("cli; hlt");
}

static void load_idt(void)
{
    struct idt_ptr idtr;
    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint64_t)idt;
    __asm__ volatile ("lidt %0" : : "m"(idtr));
}

void idt_init(void)
{
    for (int i = 0; i < IDT_ENTRIES; i++)
        idt[i].offset_low = idt[i].offset_mid = idt[i].offset_high = 0;

    for (int i = 0; i < 32; i++) {
        uint8_t ist = 0;
        switch (i) {
        case 2:  ist = IST_NMI; break;
        case 8:  ist = IST_DF;  break;
        case 18: ist = IST_MC;  break;
        default: break;
        }
        idt_set_gate(i, isr_table[i], GDT_SELECTOR_KERNEL_CS, IDT_GATE_INTERRUPT, ist);
    }

    load_idt();

    idt_set_gate(APIC_SPURIOUS_VECTOR, (uint64_t)&isr_spurious,
                 GDT_SELECTOR_KERNEL_CS, IDT_GATE_INTERRUPT, 0);

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

    setup_syscall();
}

void irq_register_handler(uint8_t irq_num, irq_handler_fn handler)
{
    if (irq_num < 16) {
        irq_handlers[irq_num] = handler;
    }
}

void irq_handler(struct interrupt_frame *frame)
{
    uint8_t irq_num = (uint8_t)(frame->int_no - 0x20);

    if (irq_num < 16 && irq_handlers[irq_num]) {
        irq_handlers[irq_num]((uint8_t)frame->int_no);
    }

    if (apic_lapic_base) {
        lapic_write(LAPIC_EOI, 0);
    }
}

void syscall_handler(struct syscall_frame *frame)
{
    switch (frame->rax) {
    case 1: { /* write(char c) */
        tty_putchar((char)frame->rdi);
        frame->rax = 0;
        break;
    }
    case 2: { /* read() */
        char c = keyboard_getchar_nb();
        frame->rax = (uint64_t)(uint8_t)c;
        break;
    }
    case 3: { /* getpid() */
        frame->rax = 0;
        break;
    }
    case 60: { /* exit(code) */
        tty_print("\n[Kernel] User program exited with code ");
        tty_print_dec((uint32_t)frame->rdi);
        tty_print("\n");

        __asm__ volatile (
            "mov %[kstack], %%rsp\n\t"
            "sti\n\t"
            "jmp kernel_idle_loop\n\t"
            :
            : [kstack] "r"((uint64_t)&kernel_interrupt_stack_top)
            : "memory"
        );
        for (;;) {}
    }
    default:
        frame->rax = (uint64_t)-1;
        break;
    }
}

void kernel_idle_loop(void)
{
    tty_print("Kernel> ");
    for (;;) {
        char c = keyboard_getchar_nb();
        if (c) {
            tty_putchar(c);
        }
        __asm__ volatile ("sti; hlt");
    }
}