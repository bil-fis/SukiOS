/* =============================================================================
 * SukiOS - 8259 PIC 初始化
 *
 * 参考：OSDev 8259 PIC
 *
 * 将主/从 PIC 的中断向量重映射到 0x20-0x2F，
 * 避免与 CPU 异常向量（0x00-0x1F）冲突。
 * 初始状态下屏蔽所有 IRQ，由 IDT 处理函数按需开启。
 * ============================================================================= */

#include "kernel/pic.h"
#include "kernel/io.h"

void pic_init(void)
{
    /* ICW1: 初始化命令（级联模式 + ICW4） */
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();

    /* ICW2: 向量偏移（主 PIC -> 0x20, 从 PIC -> 0x28） */
    outb(PIC1_DATA, IRQ_BASE_MASTER);
    io_wait();
    outb(PIC2_DATA, IRQ_BASE_SLAVE);
    io_wait();

    /* ICW3: 级联连线（主 PIC 的 IRQ2 连接从 PIC） */
    outb(PIC1_DATA, 1 << 2);     /* 主 PIC: 从 PIC 在 IRQ2 上 */
    io_wait();
    outb(PIC2_DATA, 2);          /* 从 PIC: 级联标识 = 2 */
    io_wait();

    /* ICW4: 8086 模式 */
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    /* 屏蔽所有 IRQ（除中断外不响应硬件中断） */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8)
        outb(PIC2_COMMAND, PIC_EOI);
    outb(PIC1_COMMAND, PIC_EOI);
}
