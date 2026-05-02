/* =============================================================================
 * SukiOS - LAPIC 定时器驱动
 *
 * 参考：Intel SDM Vol.3 10.5.4 (Local APIC Timer), OSDev APIC timer
 *
 * 使用 LAPIC 内置定时器（基于处理器总线时钟），支持单次触发和周期模式。
 * 通过 PIT（8254）校准确定总线频率。
 * ============================================================================= */

#ifndef SUKIOS_APIC_TIMER_H
#define SUKIOS_APIC_TIMER_H

#include <stdint.h>

/* 定时器回调函数类型 */
typedef void (*timer_callback_t)(void);

/**
 * apic_timer_init - 初始化 LAPIC 定时器
 *
 * 步骤（参考 Intel SDM Vol.3 10.5.4, OSDev APIC timer）：
 *   1. 使用 PIT 校准 LAPIC Timer 总线频率
 *   2. 配置 Divide Configuration（分频）
 *   3. 设置 LVT Timer Register（周期模式，中断向量）
 *   4. 写入 Initial Count Register 启动定时器
 */
void apic_timer_init(void);

/**
 * apic_timer_set_callback - 设置定时器中断回调
 * @cb: 回调函数（在定时器中断上下文中调用）
 *
 * 注意：回调在硬中断上下文中执行，不可阻塞/睡眠
 */
void apic_timer_set_callback(timer_callback_t cb);

/**
 * apic_timer_get_frequency - 获取定时器频率（Hz）
 * @return: 定时器中断频率
 */
uint32_t apic_timer_get_frequency(void);

/**
 * apic_timer_get_ticks - 获取定时器中断计数
 * @return: 自启动以来的中断次数
 */
uint64_t apic_timer_get_ticks(void);

/* IRQ0 中断处理函数（由 idt_stubs.asm 调用） */
void apic_timer_irq_handler(uint8_t int_no);

#endif /* SUKIOS_APIC_TIMER_H */
