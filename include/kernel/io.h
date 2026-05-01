/* =============================================================================
 * SukiOS - x86 端口 I/O + MSR 指令
 * ============================================================================= */

#ifndef SUKIOS_IO_H
#define SUKIOS_IO_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void io_wait(void)
{
    outb(0x80, 0);
}

/* 读取 MSR（Model Specific Register） */
static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

/* 写入 MSR */
static inline void wrmsr(uint32_t msr, uint64_t value)
{
    __asm__ volatile ("wrmsr"
        : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

#endif /* SUKIOS_IO_H */
