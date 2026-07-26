/*
 * include/kernel/serial.h
 * -----------------------------------------------------------------------------
 * COM1 (0x3F8) 串口调试输出接口（手册 8.1 强制要求）。
 */
#ifndef _SUKI_KERNEL_SERIAL_H
#define _SUKI_KERNEL_SERIAL_H

#include <kernel/types.h>

void serial_init(void);
void serial_write(char c);
void serial_writestr(const char *s);
void serial_write_hex(uint64_t val);
void serial_write_dec(uint64_t val);

#endif /* _SUKI_KERNEL_SERIAL_H */
