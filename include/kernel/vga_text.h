/*
 * include/kernel/vga_text.h
 * -----------------------------------------------------------------------------
 * VGA 文本模式 (0xB8000) 救砖终端（手册第 4 节备选显示）。
 * 注意：仅在显卡处于文本模式时可见。SukiOS 默认请求图形帧缓冲，
 * 故该终端作为"无帧缓冲时的回退"，以及物理机图形崩溃时的诊断输出。
 */
#ifndef _SUKI_KERNEL_VGA_TEXT_H
#define _SUKI_KERNEL_VGA_TEXT_H

#include <kernel/types.h>

#define VGA_COLS 80
#define VGA_ROWS 25

void vga_init(void);
void vga_putc(char c);
void vga_write(const char *s);

#endif /* _SUKI_KERNEL_VGA_TEXT_H */
