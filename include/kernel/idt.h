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

/* 中断帧（ISR 栈上的寄存器快照） */
struct interrupt_frame {
    /* push 顺序与 idt_stubs.asm 中的保存顺序一致 */
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t int_no;
    uint64_t err_code;
    uint64_t rip, cs, rflags;
};

void idt_set_gate(uint8_t n, uint64_t handler, uint16_t selector,
                  uint8_t type_attr, uint8_t ist);
void idt_init(void);

/* 异常处理函数（由 idt_stubs.asm 调用） */
void exception_handler(struct interrupt_frame *frame);

#endif /* SUKIOS_IDT_H */
