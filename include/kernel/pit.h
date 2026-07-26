/*
 * include/kernel/pit.h
 * -----------------------------------------------------------------------------
 * 8253/8254 可编程间隔定时器 (PIT)，IRQ0，用于驱动抢占式调度。
 */
#ifndef _SUKI_KERNEL_PIT_H
#define _SUKI_KERNEL_PIT_H

#include <kernel/types.h>

void     pit_init(uint32_t frequency_hz);
uint64_t pit_ticks(void);

#endif /* _SUKI_KERNEL_PIT_H */
