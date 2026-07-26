/*
 * include/kernel/interrupts.h
 * -----------------------------------------------------------------------------
 * 中断/异常公共定义：寄存器帧结构、IDT、处理函数注册。
 */
#ifndef _SUKI_KERNEL_INTERRUPTS_H
#define _SUKI_KERNEL_INTERRUPTS_H

#include <kernel/types.h>

/*
 * isr 汇编公共存根压栈后的寄存器帧。字段顺序必须与 isr.S 中的压栈顺序
 * 完全一致（内存低地址在前 = 最后压入者在前）。
 */
typedef struct registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;             /* 由存根压入 */
    uint64_t rip, cs, rflags, rsp, ss;     /* 由 CPU 压入 */
} registers_t;

/* IRQ 向量基址（PIC 重映射后 IRQ0 -> 32） */
#define IRQ_BASE   32
#define IRQ0       32   /* PIT 定时器 */
#define IRQ1       33   /* PS/2 键盘 */

typedef void (*isr_handler_t)(registers_t *);

void idt_init(void);
void register_interrupt_handler(uint8_t vec, isr_handler_t handler);

/* 开/关中断（隔离封装） */
void interrupts_enable(void);
void interrupts_disable(void);

#endif /* _SUKI_KERNEL_INTERRUPTS_H */
