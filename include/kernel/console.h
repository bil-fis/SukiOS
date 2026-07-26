/*
 * include/kernel/console.h
 * -----------------------------------------------------------------------------
 * 统一内核控制台：所有输出同时镜像到 COM1 串口，并输出到主显示
 * （帧缓冲图形控制台；若无帧缓冲则回退 VGA 文本模式）。
 * 提供最小 printf 实现供全内核使用。
 */
#ifndef _SUKI_KERNEL_CONSOLE_H
#define _SUKI_KERNEL_CONSOLE_H

#include <kernel/types.h>

void console_init(void);       /* 在 fb/vga 初始化之后调用 */
void kputc(char c);
void kputs(const char *s);
void kprintf(const char *fmt, ...);

/* 致命错误：输出诊断信息到串口与显示，随后停机（手册 8.1） */
__attribute__((noreturn)) void panic(const char *fmt, ...);

#endif /* _SUKI_KERNEL_CONSOLE_H */
