/*
 * kernel/arch/x86_64/apic.c
 * -----------------------------------------------------------------------------
 * 本地 APIC (LAPIC) 实现（见 include/kernel/apic.h）。
 *
 * 调用关系：kmain() -> acpi_init() 之后 -> lapic_init()；clock.c 用
 *           lapic_timer_calibrate()/lapic_timer_start() 建立系统节拍。
 *
 * 特权指令隔离：MSR 读写封装在独立 __attribute__((noinline)) 函数并加注释。
 */
#include <kernel/apic.h>
#include <kernel/acpi.h>
#include <kernel/console.h>

/* LAPIC 寄存器 MMIO 基址（运行时取自 g_acpi.lapic_phys，默认 0xFEE00000） */
static uint64_t g_lapic_base = 0xFEE00000ULL;

static inline uint32_t lapic_read(uint32_t reg)
{
    /* LAPIC 寄存器为 MP 安全的纯 MMIO 读，无副作用，不需 cli */
    return *(volatile uint32_t *)((uint8_t *)PHYS_TO_VIRT(g_lapic_base) + reg);
}

static inline void lapic_write(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)((uint8_t *)PHYS_TO_VIRT(g_lapic_base) + reg) = val;
}

/* 读取 TSC（避免严格别名问题，单独封装） */
static inline uint64_t read_tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return ((uint64_t)hi << 32) | lo;
}

/* MSR 读取封装（手册 9.2）：返回 64 位 MSR 值。 */
static __attribute__((noinline)) uint64_t msr_read(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr) : "memory");
    return ((uint64_t)hi << 32) | lo;
}

/* MSR 写入封装：输入 %ecx=msr, %eax=低32, %edx=高32；Clobber 内存。 */
static __attribute__((noinline)) void msr_write(uint32_t msr, uint64_t val)
{
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi) : "memory");
}

uint8_t lapic_init(void)
{
    g_lapic_base = g_acpi.lapic_phys ? g_acpi.lapic_phys : 0xFEE00000ULL;

    /* 1) MSR 0x1B：设置 APIC 基址并使能（保留原基址高位，仅置 EN 位） */
    uint64_t apic_base = msr_read(MSR_APIC_BASE);
    apic_base |= MSR_APIC_BASE_EN;
    msr_write(MSR_APIC_BASE, apic_base);

    /* 2) 软件使能 + 设置伪向量（SVR）。伪向量 0xFF：无源中断时投递但免 EOI。 */
    uint32_t svr = lapic_read(LAPIC_SVR);
    svr = (svr & 0xFFFFFF00u) | LAPIC_SVR_ENABLE | LAPIC_SVR_VECTOR;
    lapic_write(LAPIC_SVR, svr);

    /* 3) 屏蔽所有 LVT 条目（清掉 BIOS/固件残留配置），避免启动期误触发 */
    lapic_write(LAPIC_LVT_LINT0, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);

    /* 4) 清 pending 中断（写 EOI；若 ISR 非空再多写几次确保清空） */
    lapic_eoi();

    uint8_t id = (uint8_t)(lapic_read(LAPIC_ID) >> 24);
    uint32_t ver = lapic_read(LAPIC_VER);
    kprintf("[lapic] enabled: id=%u version=0x%x base=%p\n",
            (unsigned)id, (unsigned)ver, (void *)g_lapic_base);
    return id;
}

void lapic_eoi(void)
{
    lapic_write(LAPIC_EOI, 0);
}

uint8_t lapic_id(void)
{
    return (uint8_t)(lapic_read(LAPIC_ID) >> 24);
}

/* ---- LAPIC 定时器频率校准（由 clock.c 调用）---- */
static uint64_t g_lapic_counts_per_sec = 0;

uint64_t lapic_timer_calibrate(uint64_t tsc_hz)
{
    if (tsc_hz == 0) {
        return 0;
    }
    /* 除数：除以 1（0xB）。QEMU 下 LAPIC 总线频率与 TSC 同源，稳定可测。 */
    lapic_write(LAPIC_DIV, 0xB);

    /* 一次性模式，初始计数拉满，等待其下降固定步长，期间用 TSC 测真实时长 */
    const uint64_t window = 0x20000000ULL;   /* 测量窗口（计时单位） */
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);  /* 先屏蔽，不投递向量 */
    lapic_write(LAPIC_INIT_COUNT, 0xFFFFFFFFu);

    uint64_t tsc_a = 0, tsc_b = 0;
    tsc_a = read_tsc();
    /* 轮询当前计数直到下降 window（带最大迭代保护，避免死循环锁机） */
    uint32_t guard = 0;
    uint32_t cur;
    do {
        cur = lapic_read(LAPIC_CURR_COUNT);
        if (++guard == 0) {
            break;   /* 极端情况下放弃校准 */
        }
    } while (cur > (0xFFFFFFFFu - (uint32_t)window) && cur != 0);

    tsc_b = read_tsc();

    uint64_t consumed = 0xFFFFFFFFu - cur;     /* 实际走过的计时单位 */
    uint64_t tsc_elapsed = tsc_b - tsc_a;
    if (consumed == 0 || tsc_elapsed == 0) {
        return 0;
    }
    /* lapic_counts_per_sec = consumed / (tsc_elapsed / tsc_hz) */
    uint64_t lps = (consumed * tsc_hz) / tsc_elapsed;
    g_lapic_counts_per_sec = lps;
    kprintf("[lapic] timer calibrated: %lu counts/sec (tsc_hz=%lu)\n",
            (unsigned long)lps, (unsigned long)tsc_hz);
    return lps;
}

uint32_t lapic_timer_counts_per_sec(void)
{
    return (uint32_t)g_lapic_counts_per_sec;
}

/* ---- IPI（P0-3 SMP）---- */

/* 等待 ICR 投递完成（Delivery Status，bit12=1 表示上一条 IPI 仍在投递） */
static void lapic_icr_wait(void)
{
    uint32_t guard = 0;
    while (lapic_read(LAPIC_ICR_LOW) & (1u << 12)) {
        __asm__ volatile("pause" ::: "memory");
        if (++guard == 0) {
            break;   /* 极端超时：放弃等待，避免锁死 */
        }
    }
}

/* 写 ICR 发送 IPI：先写 HIGH（目标），再写 LOW（触发） */
static void lapic_icr_send(uint8_t apic_id, uint32_t low)
{
    lapic_write(LAPIC_ICR_HIGH, (uint32_t)apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, low);
    lapic_icr_wait();
}

void lapic_send_init(uint8_t apic_id)
{
    /* INIT assert：delivery=101b(INIT)，level=assert(bit14)，trigger=level(bit15) */
    lapic_icr_send(apic_id, 0x0000C500u);
    /* INIT deassert（部分老平台需要；QEMU/现代 CPU 兼容） */
    lapic_icr_send(apic_id, 0x00008500u);
}

void lapic_send_startup(uint8_t apic_id, uint8_t vector)
{
    /* SIPI：delivery=110b(Start-Up)，vector=物理页号（CS=vector<<8） */
    lapic_icr_send(apic_id, 0x00004600u | vector);
}

void lapic_send_ipi(uint8_t apic_id, uint8_t vector)
{
    /* 固定投递（000b）、物理目标、edge、assert */
    lapic_icr_send(apic_id, 0x00004000u | vector);
}

void lapic_broadcast_ipi(uint8_t vector)
{
    /* 目标简写 11b（bit19:18）= all excluding self，无需写 ICR_HIGH */
    lapic_write(LAPIC_ICR_LOW, 0x000C4000u | vector);
    lapic_icr_wait();
}

void lapic_timer_start(uint8_t vector, uint32_t hz)
{
    if (g_lapic_counts_per_sec == 0 || hz == 0) {
        kprintf("[lapic] timer NOT started (calibration missing)\n");
        return;
    }
    uint32_t initial = (uint32_t)(g_lapic_counts_per_sec / hz);
    if (initial == 0) {
        initial = 1;
    }
    lapic_write(LAPIC_DIV, 0xB);              /* 除以 1 */
    lapic_write(LAPIC_INIT_COUNT, initial);
    /* 周期模式 + 指定向量（不再屏蔽） */
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_PERIODIC | (uint32_t)vector);
    kprintf("[lapic] timer started: %u Hz (initial_count=%u)\n",
            (unsigned)hz, (unsigned)initial);
}
