/* =============================================================================
 * SukiOS - 8259 PIC（可编程中断控制器）
 *
 * 参考：OSDev 8259 PIC
 *
 * 将 PIC 重新映射到 0x20-0x2F，避免与 CPU 异常冲突。
 * ============================================================================= */

#ifndef SUKIOS_PIC_H
#define SUKIOS_PIC_H

#include <stdint.h>

#define PIC1_COMMAND    0x20
#define PIC1_DATA       (PIC1_COMMAND + 1)
#define PIC2_COMMAND    0xA0
#define PIC2_DATA       (PIC2_COMMAND + 1)

#define PIC_EOI         0x20

/* ICW1 位 */
#define ICW1_INIT       0x10
#define ICW1_ICW4       0x01

/* ICW4 位 */
#define ICW4_8086       0x01

/* IRQ 向量基址 */
#define IRQ_BASE_MASTER 0x20
#define IRQ_BASE_SLAVE  0x28

void pic_init(void);
void pic_send_eoi(uint8_t irq);

#endif /* SUKIOS_PIC_H */
