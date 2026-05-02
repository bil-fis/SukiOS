/* =============================================================================
 * SukiOS - 64 位 IDT（中断描述符表）
 *
 * 参考：OSDev IDT, Intel SDM Vol.3 6.14
 *
 * 64 位 IDT 条目为 16 字节，支持完整的 64 位偏移地址和 IST。
 * ============================================================================= */

#ifndef SUKIOS_IDT_H
#define SUKIOS_IDT_H

#include <stdint.h>

#define IDT_ENTRIES 256

/* 64 位 IDT 门描述符 */
struct idt_entry {
    uint16_t offset_low;     /* 偏移 bits 0-15 */
    uint16_t selector;       /* 代码段选择子 */
    uint8_t  ist;            /* IST 偏移 (bits 0-2)，0 表示不使用 IST */
    uint8_t  type_attr;      /* 类型属性 */
    uint16_t offset_mid;     /* 偏移 bits 16-31 */
    uint32_t offset_high;    /* 偏移 bits 32-63 */
    uint32_t reserved;       /* 保留，置零 */
} __attribute__((packed));

/* IDT 指针（用于 LIDT 指令） */
struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* 门类型属性 */
#define IDT_GATE_INTERRUPT 0x8E   /* 中断门（自动清除 IF） */
#define IDT_GATE_TRAP      0x8F   /* 陷阱门（不清除 IF） */

/* 中断帧（ISR 栈上的寄存器快照）
 * 布局与 idt_stubs.asm 中的压栈顺序严格对应。
 *
 * ISR 存根压栈顺序（先压的在低地址，后压的在高地址）：
 *   push rax   → 偏移 15*8 = 120
 *   push rbx   → 偏移 14*8 = 112
 *   ...
 *   push r15   → 偏移 0
 *   mov rdi, rsp → RDI 指向 r15
 *
 * 因此结构体中 r15 在 offset 0，rax 在 offset 15*8。
 * 当中断发生特权级切换（ring3→ring0）时，CPU 额外压入 SS 和 RSP。
 * 参考：Intel SDM Vol.3 Figure 6-8 (64-bit mode interrupt stack frame) */
struct interrupt_frame {
    /* 由 ISR 存根压入的通用寄存器（r15 在栈顶 = offset 0） */
    uint64_t r15, r14, r13, r12, r11, r10;
    uint64_t r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rax;
    /* 由 ISR 存根压入 */
    uint64_t int_no;
    uint64_t err_code;        /* 无错误码的异常压入 0 占位 */
    /* 由 CPU 自动压入 */
    uint64_t rip, cs, rflags;
    /* 以下仅在特权级切换时（CS 低 2 位 != 0）由 CPU 压入，否则为无效值 */
    uint64_t rsp, ss;
};

void idt_set_gate(uint8_t n, uint64_t handler, uint16_t selector,
                  uint8_t type_attr, uint8_t ist);
void idt_init(void);

/* 异常处理函数（由 idt_stubs.asm 调用） */
void exception_handler(struct interrupt_frame *frame);

/* IRQ 通用处理函数（由 idt_stubs.asm 的 irq_common_stub 调用）
 * 根据 int_no 分发到具体驱动处理函数 */
void irq_handler(struct interrupt_frame *frame);

/* 注册 IRQ 处理函数
 * @irq_num: IRQ 编号 (0-15)
 * @handler: 处理函数，参数为中断号 */
typedef void (*irq_handler_fn)(uint8_t int_no);
void irq_register_handler(uint8_t irq_num, irq_handler_fn handler);

/* ---- syscall/sysret 支持 ----
 * 参考：AMD64 APM Vol.2 12.4.1, Intel SDM Vol.3 4.9
 *
 * syscall 存根在 idt_stubs.asm 中定义：
 *   - 保存用户态寄存器到 struct syscall_frame
 *   - 调用 C 语言 syscall_handler
 *   - 恢复寄存器并通过 sysretq 返回 Ring 3
 *
 * syscall 约定（Linux x86_64 ABI）：
 *   RAX = 系统调用编号
 *   RDI, RSI, RDX, R10, R8, R9 = 参数
 *   返回值在 RAX
 *   syscall 指令自动保存 RIP→RCX, RFLAGS→R11 */

struct syscall_frame {
    /* 由 syscall 存根压入的通用寄存器 */
    uint64_t rax;          /* 系统调用编号 / 返回值 */
    uint64_t rdi;          /* 第 1 个参数 */
    uint64_t rsi;          /* 第 2 个参数 */
    uint64_t rdx;          /* 第 3 个参数 */
    uint64_t r10;          /* 第 4 个参数 */
    uint64_t r8;           /* 第 5 个参数 */
    uint64_t r9;           /* 第 6 个参数 */
    /* 由 CPU 自动保存（syscall 指令） */
    uint64_t rcx;          /* 返回地址（RIP） */
    uint64_t r11;          /* 保存的 RFLAGS */
    uint64_t rip;          /* 返回地址副本（用于调试） */
};

void syscall_handler(struct syscall_frame *frame);

/* Ring 3 切换存根（在 idt_stubs.asm 中定义）
 * 通过 iretq 切换到 Ring 3 用户态 */
extern void enter_ring3(uint64_t rip, uint64_t rsp);

#endif /* SUKIOS_IDT_H */
