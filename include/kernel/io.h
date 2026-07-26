/*
 * include/kernel/io.h
 * -----------------------------------------------------------------------------
 * 端口 I/O 原语。in/out 指令为最基础的硬件访问方式，作为 static inline 提供。
 * 注：手册 9.2 要求隔离的是 wrmsr/lgdt/ltr 等会改变 CPU 全局状态的特权指令，
 * 端口读写不在此列，内联使用是通行做法。
 */
#ifndef _SUKI_KERNEL_IO_H
#define _SUKI_KERNEL_IO_H

#include <kernel/types.h>

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val)
{
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(uint16_t port, uint32_t val)
{
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* 向 0x80 端口写入用于产生一小段 I/O 延迟 */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

#endif /* _SUKI_KERNEL_IO_H */
