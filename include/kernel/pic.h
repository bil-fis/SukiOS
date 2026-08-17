/*
 * include/kernel/pic.h
 * -----------------------------------------------------------------------------
 * 8259A 可编程中断控制器 (PIC) 重映射与 EOI。
 */
#ifndef _SUKI_KERNEL_PIC_H
#define _SUKI_KERNEL_PIC_H

#include <kernel/types.h>

void pic_remap(void);                 /* 将 IRQ0..15 重映射到向量 32..47 */
void pic_send_eoi(uint8_t irq);       /* 中断结束应答 */
void pic_set_mask(uint8_t irq);       /* 屏蔽某 IRQ */
void pic_clear_mask(uint8_t irq);     /* 放开某 IRQ */
void pic_disable(void);                /* P0-2：切换到 APIC 后屏蔽整个 8259 */

#endif /* _SUKI_KERNEL_PIC_H */
