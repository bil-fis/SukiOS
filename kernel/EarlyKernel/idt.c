/* =============================================================================
 * SukiOS - 64 位 IDT 初始化 + CPU 异常处理 + int 0x80 系统调用
 *
 * 使用 int 0x80 替代 syscall，兼容性更好。
 * ============================================================================= */

#include "kernel/gdt.h"
#include "kernel/idt.h"
#include "kernel/tty.h"
#include "kernel/io.h"
#include "kernel/apic.h"
#include "kernel/keyboard.h"
#include "kernel/proc.h"
#include "kernel/vfs.h"
#include "kernel/fat32.h"
#include "kernel/heap.h"

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
extern uint64_t isr80;   // int 0x80 存根

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

/* ---- int 0x80 处理函数 ---- */
static void int80_handler(struct interrupt_frame *frame)
{
    struct syscall_frame sf;
    sf.rax = frame->rax;
    sf.rdi = frame->rdi;
    sf.rsi = frame->rsi;
    sf.rdx = frame->rdx;
    sf.r10 = frame->r10;
    sf.r8  = frame->r8;
    sf.r9  = frame->r9;
    sf.rcx = 0;
    sf.r11 = 0;
    sf.rip = 0;

    syscall_handler(&sf);
    frame->rax = sf.rax;   // 将返回值写回用户态 rax
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
    /* 系统调用分发 */
    if (frame->int_no == 0x80) {
        int80_handler(frame);
        return;       // 处理完毕，直接返回，不打印异常
    }

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

    /* 注册 int 0x80 系统调用门（DPL=3，中断门） */
    idt_set_gate(0x80, (uint64_t)&isr80, GDT_SELECTOR_KERNEL_CS, 0xEE, 0);
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

void syscall_handler(struct syscall_frame *frame) {
    pcb_t *curr = g_current_proc;

    switch (frame->rax) {
    case 1: { /* write(char) */
            tty_putchar((char)frame->rdi);
            frame->rax = 0;
            break;
    }
    case 2: { /* read() */
            char c = (unsigned char)keyboard_getchar_blocking();
            frame->rax = (uint64_t)c;
            break;
    }
    case 3: { /* getpid() */
            if (curr) frame->rax = curr->pid;
            else frame->rax = 0;
            break;
    }
    case 59: { /* execve(path, argv, envp) */
            // execve 系统调用：替换当前进程为新程序
            // 参数：rdi = path (const char*), rsi = argv (char**), rdx = envp (char**)
            const char *path = (const char*)frame->rdi;
            
            // 验证路径指针有效性（简单检查）
            if (!path || (uint64_t)path < 0x100000) {
                frame->rax = (uint64_t)-1;
                break;
            }
            
            // 尝试打开文件
            int fd = vfs_open(path, O_RDONLY);
            if (fd < 0) {
                frame->rax = (uint64_t)-2; // ENOENT
                break;
            }
            
            size_t file_size = vfs_get_size(fd);
            if (file_size == 0) {
                vfs_close(fd);
                frame->rax = (uint64_t)-1;
                break;
            }
            
            // 分配内存读取文件
            uint8_t *elf_buf = (uint8_t *)kmalloc(file_size);
            if (!elf_buf) {
                vfs_close(fd);
                frame->rax = (uint64_t)-12; // ENOMEM
                break;
            }
            
            // 读取ELF文件
            if (vfs_read(fd, elf_buf, file_size) != (int)file_size) {
                kfree(elf_buf);
                vfs_close(fd);
                frame->rax = (uint64_t)-1;
                break;
            }
            
            vfs_close(fd);
            
            // 创建新进程
            pcb_t *new_proc = proc_create_from_elf(elf_buf, file_size);
            kfree(elf_buf);
            
            if (!new_proc) {
                frame->rax = (uint64_t)-1;
                break;
            }
            
            // 替换当前进程（简单实现：退出当前进程，新进程会运行）
            if (curr) {
                curr->state = PROC_ZOMBIE;
                // 释放当前进程资源（简化版）
                if (curr->kernel_stack) {
                    kfree(curr->kernel_stack);
                }
                // 清理文件描述符表
                for (int i = 0; i < MAX_FD; i++) {
                    if (curr->fds[i].vnode) {
                        vfs_close_node(curr->fds[i].vnode);
                        curr->fds[i].vnode = NULL;
                    }
                }
            }
            
            // 设置新进程为当前进程
            g_current_proc = new_proc;
            new_proc->state = PROC_RUNNING;
            
            frame->rax = 0; // success
            break;
    }
    case 60: { /* exit(code) */
            int code = (int)frame->rdi;
            tty_print("[exit] PID ");
            if (curr) tty_print_dec((uint32_t)curr->pid);
            tty_print(" exited with ");
            tty_print_dec(code);
            tty_print("\n");
            if (curr) curr->state = PROC_ZOMBIE;
            schedule();
            break;
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