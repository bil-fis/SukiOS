/*
 * kernel/arch/x86_64/pic.c
 * -----------------------------------------------------------------------------
 * 8259A PIC 重映射。BIOS 默认将 IRQ0..7 映射到向量 8..15，与 CPU 异常冲突，
 * 必须重映射到 32..47。
 *
 * 调用关系：kmain() -> pic_remap()；isr_dispatch() -> pic_send_eoi()。
 */
#include <kernel/pic.h>
#include <kernel/io.h>

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1

#define ICW1_INIT  0x11   /* 初始化 + 需要 ICW4 */
#define ICW4_8086  0x01   /* 8086/88 模式 */
#define PIC_EOI    0x20

void pic_remap(void)
{
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    outb(PIC1_CMD, ICW1_INIT); io_wait();
    outb(PIC2_CMD, ICW1_INIT); io_wait();
    outb(PIC1_DATA, 0x20);     io_wait();   /* 主片向量偏移 = 32 */
    outb(PIC2_DATA, 0x28);     io_wait();   /* 从片向量偏移 = 40 */
    outb(PIC1_DATA, 0x04);     io_wait();   /* 告知主片：从片接在 IRQ2 */
    outb(PIC2_DATA, 0x02);     io_wait();   /* 告知从片其级联身份 */
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    /* 恢复原掩码（此处全部先屏蔽，由各驱动按需放开） */
    (void)mask1; (void)mask2;
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8) {
        outb(PIC2_CMD, PIC_EOI);
    }
    outb(PIC1_CMD, PIC_EOI);
}

/* P0-2：切换到 APIC 后彻底屏蔽 8259 PIC，避免其与 I/O APIC 双投递。
 * 仅置数据端口屏蔽位即可（不再依赖 ICW 重映射）。 */
void pic_disable(void)
{
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_set_mask(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8) irq -= 8;
    uint8_t v = inb(port) | (1 << irq);
    outb(port, v);
}

void pic_clear_mask(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8) irq -= 8;
    uint8_t v = inb(port) & ~(1 << irq);
    outb(port, v);
}
