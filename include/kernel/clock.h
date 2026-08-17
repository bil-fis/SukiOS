/*
 * include/kernel/clock.h
 * -----------------------------------------------------------------------------
 * 高精度时钟体系（P0-4）：TSC 校准 + LAPIC 本地定时器作为系统节拍 + HPET 探测。
 *
 * 设计目标（对标现代 OS）：
 *   - 单调时钟：clock_monotonic_ns() 基于已校准的 invariant TSC，纳秒级分辨率，
 *     供 clock_gettime(CLOCK_MONOTONIC) 等语义（后续 syscall 接入）。
 *   - 系统节拍：取代 PIT 100Hz，改用 LAPIC 本地定时器（每 CPU，SMP 友好）。
 *   - HPET：从 ACPI HPET 表探测基址并记录，作为备用高精时钟源（本阶段仅探测
 *     与日志；真正切换到 HPET 计时留待后续，LAPIC 定时器已足够驱动调度）。
 *
 * 调用关系：kmain() -> clock_init()（须 acpi_init + lapic_init 之后、
 *           interrupts_enable 之前，因为校准过程关中断）。
 */
#ifndef _SUKI_KERNEL_CLOCK_H
#define _SUKI_KERNEL_CLOCK_H

#include <kernel/types.h>
#include <kernel/interrupts.h>   /* registers_t：sched_tick 节拍钩子入参 */

/* 系统节拍钩子：每 CPU LAPIC 定时器(100Hz)或 PIT 触发后调用（弱符号，
 * 由调度器 sched.c 提供强实现覆盖 pit.c 的空弱实现）。声明于此供 clock.c
 * 等调用方获得正确原型，避免隐式声明（UB/错误调用约定）。 */
void sched_tick(registers_t *r);

/* 初始化时钟子系统：
 *   1) 用 PIT 一次性模式校准 TSC 频率（关中断，安全）；
 *   2) 用 TSC 校准 LAPIC 定时器频率；
 *   3) 启动 LAPIC 定时器作为 100Hz 系统节拍（向量 IRQ0），注册节拍 handler。
 * 返回是否成功（失败则系统无法获得节拍，但通常不会）。 */
bool clock_init(void);

/* 单调递增纳秒计数（基于 TSC）。单调、不回退（同一次启动内）。 */
uint64_t clock_monotonic_ns(void);

/* 已校准的 TSC 频率（Hz）。 */
uint64_t clock_tsc_freq_hz(void);

/* HPET 探测结果（0 = 未找到）。 */
uint64_t clock_hpet_base(void);

#endif /* _SUKI_KERNEL_CLOCK_H */
