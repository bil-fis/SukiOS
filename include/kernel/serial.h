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

/* 非阻塞接收接口（供 Ring3 INPUT_SERVER 轮询串口控制台输入，headless QEMU
 * 下经 -serial 注入键，真实部署下经物理串口控制台均可使用）。 */
bool serial_read_ready(void);   /* 接收缓冲是否有数据 */
int  serial_read(void);         /* 返回 0..255，无数据时返回 -1 */

#endif /* _SUKI_KERNEL_SERIAL_H */
