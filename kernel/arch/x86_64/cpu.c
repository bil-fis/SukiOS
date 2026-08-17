/*
 * kernel/arch/x86_64/cpu.c
 * -----------------------------------------------------------------------------
 * CPU 控制原语（中断开关等）。sti/cli 影响全局中断标志，按手册 9.2 精神
 * 隔离在独立函数中。
 */
#include <kernel/interrupts.h>

void interrupts_enable(void)
{
    __asm__ volatile("sti" ::: "memory");
}

void interrupts_disable(void)
{
    __asm__ volatile("cli" ::: "memory");
}
