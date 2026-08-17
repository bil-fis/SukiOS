/*
 * kernel/arch/x86_64/pit.c
 * -----------------------------------------------------------------------------
 * PIT 定时器：产生周期性 IRQ0 中断，驱动系统节拍与抢占式调度器。
 *
 * 调用关系：kmain() -> pit_init(); IRQ0 -> pit_irq_handler() -> (阶段五) sched_tick()。
 */
#include <kernel/pit.h>
#include <kernel/interrupts.h>
#include <kernel/pic.h>
#include <kernel/io.h>
#include <kernel/console.h>

#define PIT_CHANNEL0  0x40
#define PIT_CMD       0x43
#define PIT_FREQ      1193182   /* 输入时钟 Hz */

static volatile uint64_t g_ticks = 0;
static uint32_t g_hz = 100;

/* 阶段五调度器就绪后由其接管；弱符号便于覆盖 */
__attribute__((weak)) void sched_tick(registers_t *r) { (void)r; }

static void pit_irq_handler(registers_t *r)
{
    g_ticks++;
    sched_tick(r);   /* 交给调度器（阶段五前为空实现） */
}

void pit_init(uint32_t frequency_hz)
{
    if (frequency_hz == 0) {
        frequency_hz = 100;
    }
    g_hz = frequency_hz;
    uint32_t divisor = PIT_FREQ / frequency_hz;
    if (divisor > 0xFFFF) {
        divisor = 0xFFFF;
    }

    outb(PIT_CMD, 0x36);                       /* 通道0, lo/hi, 方波模式3 */
    outb(PIT_CHANNEL0, divisor & 0xFF);
    outb(PIT_CHANNEL0, (divisor >> 8) & 0xFF);

    register_interrupt_handler(IRQ0, pit_irq_handler);
    pic_clear_mask(0);                         /* 放开 IRQ0 */

    kprintf("[pit] timer @ %u Hz (divisor=%u)\n", frequency_hz, divisor);
}

uint64_t pit_ticks(void)
{
    return g_ticks;
}
