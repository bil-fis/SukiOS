/*
 * include/kernel/apic.h
 * -----------------------------------------------------------------------------
 * 本地 APIC (LAPIC) 驱动（xAPIC MMIO 模式）。
 *
 * P0-2 的一部分：取代 8259 PIC + PIT，提供每 CPU 的中断控制器与本地定时器。
 * 多核（P0-3）时每 CPU 一个 LAPIC，但当前为单 BSP 阶段，仅启用 BSP 的 LAPIC。
 *
 * 寄存器访问：LAPIC 是 MMIO，基址来自 MSR 0x1B（默认 0xFEE00000，ACPI MADT
 * 也给出）。OSDev APIC 文档要求该窗口必须是 strong uncacheable（PCD+PWT）。
 * lapic_init 经 vmm_map_page 把物理页重映射到专用 UC 虚拟窗口（0xFFFF8000000FE000）
 * 后，所有寄存器访问只走该 UC 窗口；引导期 WB 恒等映射不再用于 LAPIC。
 */
#ifndef _SUKI_KERNEL_APIC_H
#define _SUKI_KERNEL_APIC_H

#include <kernel/types.h>

/* LAPIC 寄存器偏移（MMIO，4 字节访问） */
#define LAPIC_ID        0x020
#define LAPIC_VER       0x030
#define LAPIC_TPR       0x080
#define LAPIC_EOI       0x0B0
#define LAPIC_LDR       0x0D0
#define LAPIC_DFR       0x0E0
#define LAPIC_SVR       0x0F0   /* Spurious Vector Register */
#define LAPIC_ISR0      0x100   /* In-Service 寄存器（只读） */
#define LAPIC_ICR_LOW   0x300   /* 中断命令寄存器低 32 位（写触发 IPI） */
#define LAPIC_ICR_HIGH  0x310   /* 中断命令寄存器高 32 位（目标 LAPIC ID<<24） */
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_LVT_ERROR 0x370
#define LAPIC_INIT_COUNT 0x380
#define LAPIC_CURR_COUNT 0x390
#define LAPIC_DIV       0x3E0

/* SVR 标志位 */
#define LAPIC_SVR_ENABLE  (1u << 8)     /* 第 8 位：APIC 软件使能 */
#define LAPIC_SVR_VECTOR  0xFF          /* 伪向量（无源中断时投递，不需 EOI） */

/* LVT 定时器标志 */
#define LAPIC_LVT_MASKED  (1u << 16)    /* 屏蔽该 LVT 条目 */
#define LAPIC_LVT_PERIODIC (1u << 17)   /* 周期模式（否则一次性） */

/* MSR */
#define MSR_APIC_BASE     0x1B
#define MSR_APIC_BASE_EN  (1u << 11)    /* 第 11 位：LAPIC 全局使能 */
#define MSR_APIC_BASE_BSP (1u << 8)     /* 第 8 位：当前为 BSP */

/* 初始化本地 APIC：使能（MSR 0x1B）、设置 SVR、屏蔽所有 LVT（避免 BIOS
 * 残留配置误触发）、清 pending。返回本 CPU 的 LAPIC ID。 */
uint8_t lapic_init(void);

/* 向本 LAPIC 写 EOI（中断处理结束时必须调用，对本地 LVT 与 IOAPIC 投递都有效） */
void lapic_eoi(void);

/* 读本 CPU 的 LAPIC ID */
uint8_t lapic_id(void);

/* 配置并启动本地定时器：周期模式，每 1/hz 秒触发一次 vector 向量。
 * 频率通过 TSC 校准得到（见 clock.c 的 lapic_timer_calibrate）。 */
void lapic_timer_start(uint8_t vector, uint32_t hz);

/* 提供给 clock.c：用 TSC 校准 LAPIC 定时器频率（次/秒）。
 * 返回 0 表示校准失败。 */
uint64_t lapic_timer_calibrate(uint64_t tsc_hz);

/* 当前 LAPIC 定时器初始计数值（校准结果，供 lapic_timer_start 使用） */
uint32_t lapic_timer_counts_per_sec(void);

/* ---- IPI（P0-3 SMP）---- */
/* 向目标 LAPIC 发送 INIT（assert + deassert，MP 规范唤醒序列第一步） */
void lapic_send_init(uint8_t apic_id);
/* 向目标 LAPIC 发送 STARTUP IPI（vector = 目标物理页号，页须 <1MB 且 4K 对齐） */
void lapic_send_startup(uint8_t apic_id, uint8_t vector);
/* 向单个目标发送固定向量 IPI */
void lapic_send_ipi(uint8_t apic_id, uint8_t vector);
/* 向除自己外所有 CPU 广播固定向量 IPI（目标简写 11b=all-excluding-self） */
void lapic_broadcast_ipi(uint8_t vector);

#endif /* _SUKI_KERNEL_APIC_H */
