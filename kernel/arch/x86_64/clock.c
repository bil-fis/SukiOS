/*
 * kernel/arch/x86_64/clock.c
 * -----------------------------------------------------------------------------
 * 高精度时钟实现（见 include/kernel/clock.h）。
 *
 * 校准链路：PIT(已知 1193182Hz) 作基准 -> 测 TSC 频率 -> 测 LAPIC 定时器频率。
 * 全程关中断，纯轮询，避免中断干扰导致测量失真或死锁。
 */
#include <kernel/clock.h>
#include <kernel/apic.h>
#include <kernel/acpi.h>
#include <kernel/interrupts.h>
#include <kernel/io.h>
#include <kernel/console.h>

#define PIT_CHANNEL0  0x40
#define PIT_CMD       0x43
#define PIT_FREQ      1193182ULL

static uint64_t g_tsc_hz = 0;
static uint64_t g_hpet_base = 0;

/* ---- TSC 读取封装（rdtsc，不串行化；校准场景够用）---- */
static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return ((uint64_t)hi << 32) | lo;
}

/* ---- 用 PIT 一次性模式校准 TSC 频率 ----
 * 步骤：把 PIT 通道0 设为模式0（oneshot）、初值 0xFFFF，记录 tsc0；轮询其
 * 计数到 0（OUT 位置位），记录 tsc1。PIT 走完 0x10000 个计数，耗时
 * 0x10000/PIT_FREQ 秒。tsc_hz = (tsc1-tsc0) / 该时长。 */
static uint64_t tsc_calibrate_via_pit(void)
{
    /* 通道0，访问 lo/hi，模式0(oneshot) */
    outb(PIT_CMD, 0x30);
    outb(PIT_CHANNEL0, 0xFF);
    outb(PIT_CHANNEL0, 0xFF);

    uint64_t tsc0 = rdtsc();

    /* 轮询 PIT 状态（发 0xE2 锁存通道0状态，读回；bit7=OUT 完成） */
    uint32_t guard = 0;
    uint8_t status;
    do {
        outb(PIT_CMD, 0xE2);
        status = inb(PIT_CHANNEL0);
        if (++guard == 0) {
            return 0;   /* 超时保护 */
        }
    } while ((status & 0x80) == 0);

    uint64_t tsc1 = rdtsc();
    if (tsc1 <= tsc0) {
        return 0;
    }
    uint64_t elapsed_tsc = tsc1 - tsc0;
    /* 0x10000 计数 @ PIT_FREQ -> 秒 = 0x10000 / PIT_FREQ */
    __uint128_t num = (__uint128_t)elapsed_tsc * PIT_FREQ;
    uint64_t hz = (uint64_t)(num / 0x10000ULL);

    /* 合理性裁剪：100MHz..10GHz，排除明显失准 */
    if (hz < 100000000ULL || hz > 10000000000ULL) {
        kprintf("[clock] TSC calibration out of range (%lu Hz), discarding\n",
                (unsigned long)hz);
        return 0;
    }
    return hz;
}

/* LAPIC 定时器节拍 handler：仅调用调度器节拍钩子（与旧 pit_irq_handler 等价） */
static void lapic_timer_handler(registers_t *r)
{
    sched_tick(r);   /* 弱符号，由调度器覆盖 */
}

bool clock_init(void)
{
    /* 1) 关中断下校准 TSC（此时 interrupts_enable 尚未调用） */
    g_tsc_hz = tsc_calibrate_via_pit();
    if (g_tsc_hz == 0) {
        kprintf("[clock] TSC calibration FAILED\n");
        return false;
    }
    kprintf("[clock] TSC freq = %lu Hz (%lu MHz)\n",
            (unsigned long)g_tsc_hz,
            (unsigned long)(g_tsc_hz / 1000000ULL));

    /* 2) 用 TSC 校准 LAPIC 定时器频率 */
    uint64_t lps = lapic_timer_calibrate(g_tsc_hz);
    if (lps == 0) {
        kprintf("[clock] LAPIC timer calibration FAILED\n");
        return false;
    }

    /* 3) 启动 LAPIC 定时器作为 100Hz 节拍，向量 = IRQ0(32) */
    register_interrupt_handler(IRQ0, lapic_timer_handler);
    lapic_timer_start((uint8_t)IRQ0, 100);

    /* 4) HPET 探测（仅记录基址 + 日志，本阶段不作为节拍源） */
    uint64_t hpet = acpi_find_table("HPET");
    if (hpet) {
        /* HPET ACPI 表布局（OSDev HPET 条目）：
         *   description_table_header 36 字节
         *   + hardware_rev_id(1) + bitfield(1) + pci_vendor_id(2)  = 4 字节
         *   + address_structure(GAS, 12 字节)，GAS 内 address 字段在 GAS 偏移 4。
         * 故 address 绝对偏移 = 36 + 4 + 4 = 44。取 8 字节完整 MMIO 地址。 */
        const uint8_t *p = (const uint8_t *)PHYS_TO_VIRT(hpet);
        g_hpet_base = *(const uint64_t *)(p + 44);
        kprintf("[clock] HPET detected @ %p\n", (void *)g_hpet_base);
    } else {
        kprintf("[clock] HPET not present\n");
    }

    kprintf("[clock] high-precision clock ready (TSC + LAPIC timer 100Hz)\n");
    return true;
}

uint64_t clock_monotonic_ns(void)
{
    if (g_tsc_hz == 0) {
        return 0;
    }
    return (rdtsc() * 1000000000ULL) / g_tsc_hz;
}

uint64_t clock_tsc_freq_hz(void)
{
    return g_tsc_hz;
}

uint64_t clock_hpet_base(void)
{
    return g_hpet_base;
}
