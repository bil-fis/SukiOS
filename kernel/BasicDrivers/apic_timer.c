/* =============================================================================
 * SukiOS - LAPIC 定时器驱动实现
 *
 * 参考：Intel SDM Vol.3 10.5.4 (Local APIC Timer), OSDev APIC timer
 *
 * 校准方法（参考 OSDev PIT, OSDev APIC timer - Calibrating the APIC timer）：
 *   1. 使用 PIT 通道 0（系统定时器）产生精确的时间基准（~10ms）
 *   2. 在此期间让 LAPIC Timer 以最大初始值运行
 *   3. 读取 Current Count，计算总线频率
 *
 * 注意：使用 PIT 通道 0 而非通道 2，因为通道 2（扬声器）在某些环境下不可靠。
 *       分频值会影响精度，使用 Divide=16 获得较好的范围。
 * ============================================================================= */

#include "kernel/apic_timer.h"
#include "kernel/apic.h"
#include "kernel/tty.h"
#include "kernel/io.h"
#include <stdint.h>
#include <stdbool.h>
#include "kernel/proc.h"

/* ---- PIT (8254 Timer) 寄存器/命令 ---- */
/* 参考：OSDev PIT, Intel 8253/8254 datasheet */
#define PIT_CHANNEL0    0x40    /* PIT 通道 0（系统定时器） */
#define PIT_CMD_PORT    0x43    /* PIT 命令端口 */

/* PIT 命令字节构造 */
#define PIT_CMD_CH0     0x00    /* 选择通道 0 (bits 7-6) */
#define PIT_CMD_LATCH   0x00    /* 锁存命令 (bits 5-4) */
#define PIT_CMD_LOHI    0x30    /* 先低后高字节 (bits 5-4) */
#define PIT_CMD_RATE    0x06    /* 模式 2: 速率发生器 (bits 3-1) */

/* PIT 时钟频率 */
#define PIT_FREQ_HZ     1193182 /* PIT 输入时钟频率 (Hz) */

/* ---- 定时器状态 ---- */
static volatile uint64_t timer_ticks = 0;
static uint32_t          timer_bus_freq = 0;  /* 总线频率 (Hz) */
static uint32_t          timer_freq_hz = 0;   /* 定时器中断频率 */
static timer_callback_t  timer_cb = NULL;

/* =========================================================================
 * PIT 辅助函数（仅在校准时使用）
 * 参考：OSDev PIT
 * ========================================================================= */

/* 读取 PIT 通道 0 的当前计数值（16 位）
 * 先锁存再读取低、高字节 */
static uint16_t pit_read_channel0(void)
{
    uint16_t count;
    outb(PIT_CMD_PORT, PIT_CMD_CH0 | PIT_CMD_LATCH);
    count  = inb(PIT_CHANNEL0);
    count |= (uint16_t)inb(PIT_CHANNEL0) << 8;
    return count;
}

/* 重新配置 PIT 通道 0 为模式 2（速率发生器），最大计数值 */
static void pit_channel0_setup(void)
{
    /* 通道 0, 先低后高, 模式 2, 16 位二进制 */
    outb(PIT_CMD_PORT, PIT_CMD_CH0 | PIT_CMD_LOHI | PIT_CMD_RATE);
    outb(PIT_CHANNEL0, 0xFF);
    outb(PIT_CHANNEL0, 0xFF);
}

/* =========================================================================
 * LAPIC Timer 校准
 *
 * 使用 PIT 通道 0 作为精确时间基准：
 *   1. 配置 PIT 通道 0 为模式 2，计数值 0xFFFF（约 54.9ms 一个周期）
 *   2. 启动 LAPIC Timer 以最大计数值运行
 *   3. 等待 PIT 通道 0 减少指定量（对应精确的时间间隔）
 *   4. 读取 LAPIC Timer 剩余计数值，计算总线频率
 *
 * 参考：OSDev APIC timer - Calibrating the APIC timer
 * ========================================================================= */

static uint32_t calibrate_apic_timer(void)
{
    /* 使用 Divide=16 获得较好的计数范围 */
    lapic_write(LAPIC_DIV_CONF, LAPIC_DIV_16);

    /* 屏蔽定时器中断（校准期间不需要中断） */
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASK);

    /* 设置最大初始计数值 */
    lapic_write(LAPIC_INIT_COUNT, 0xFFFFFFFF);

    /* 重新配置 PIT 通道 0 并读取起始计数值 */
    pit_channel0_setup();
    uint16_t pit_start = pit_read_channel0();

    /* 等待 PIT 通道 0 减少约 11932 个计数（≈ 10ms）
     * 11932 = PIT_FREQ_HZ / 100 */
    uint16_t pit_target = (uint16_t)(PIT_FREQ_HZ / 100);

    while (true) {
        uint16_t pit_now = pit_read_channel0();
        uint16_t delta = pit_start - pit_now;  /* 无符号减法自动处理回绕 */
        if (delta >= pit_target)
            break;
    }

    /* 停止 LAPIC Timer */
    lapic_write(LAPIC_INIT_COUNT, 0);

    /* 读取剩余计数值 */
    uint32_t ticks_elapsed = 0xFFFFFFFF - lapic_read(LAPIC_CUR_COUNT);

    /* 计算总线频率
     * ticks_elapsed 是 Divide=16 下 10ms 内经过的计数
     * 总线频率 = ticks_elapsed * 100 (10ms -> Hz) * 16 (分频因子) */
    uint32_t bus_freq = ticks_elapsed * 100 * 16;

    return bus_freq;
}

/* =========================================================================
 * 公共接口
 * ========================================================================= */

void apic_timer_irq_handler(uint8_t int_no) {
    (void)int_no;
    timer_ticks++;

    if (timer_cb)
        timer_cb();

    schedule();             // 抢占式调度
}

void apic_timer_set_callback(timer_callback_t cb)
{
    timer_cb = cb;
}

uint64_t apic_timer_get_ticks(void)
{
    return timer_ticks;
}

uint32_t apic_timer_get_frequency(void)
{
    return timer_freq_hz;
}

void apic_timer_init(void)
{
    /* 1. 校准 LAPIC Timer 获取总线频率 */
    timer_bus_freq = calibrate_apic_timer();

    if (timer_bus_freq == 0) {
        tty_setcolor(VGA_RED, VGA_BLACK);
        tty_print("  ERROR: LAPIC Timer calibration failed!\n");
        tty_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    /* 2. 配置 Divide=16（与校准时一致） */
    lapic_write(LAPIC_DIV_CONF, LAPIC_DIV_16);

    /* 3. 设置定时器频率 = 100 Hz（10ms 间隔）
     * Initial Count = 总线频率 / 分频 / 目标频率 */
    uint32_t target_freq = 100;  /* Hz */
    uint32_t init_count = timer_bus_freq / 16 / target_freq;

    /* 确保 Initial Count 不超过 32 位范围 */
    if (init_count == 0)
        init_count = 1;
    if (init_count > 0xFFFFFFFF)
        init_count = 0xFFFFFFFF;

    timer_freq_hz = target_freq;

    /* 4. 设置 LVT Timer Register
     *   Vector = IRQ_APIC_TIMER (0x20)
     *   Delivery Mode = Fixed (bits 8-10 = 000)
     *   Mask = 0 (启用中断)
     *   Timer Mode = Periodic (bit 17 = 1)
     * 参考：Intel SDM Vol.3 10.5.4 Table 10-9 */
    uint32_t lvt = IRQ_APIC_TIMER;        /* 中断向量 */
    lvt |= LAPIC_LVT_PERIODIC;            /* 周期模式 */
    lapic_write(LAPIC_LVT_TIMER, lvt);

    /* 5. 写入 Initial Count 启动定时器 */
    lapic_write(LAPIC_INIT_COUNT, init_count);

    tty_print("[OK] LAPIC Timer: bus_freq=");
    tty_print_dec(timer_bus_freq / 1000000);
    tty_print(" MHz  freq=");
    tty_print_dec(timer_freq_hz);
    tty_print(" Hz  init_count=");
    tty_print_dec(init_count);
    tty_print("\n");
}
