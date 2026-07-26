/*
 * include/kernel/framebuffer.h
 * -----------------------------------------------------------------------------
 * 32 位色深线性帧缓冲驱动 + 图形文本控制台（手册第 4 节双轨显示之主显示）。
 */
#ifndef _SUKI_KERNEL_FRAMEBUFFER_H
#define _SUKI_KERNEL_FRAMEBUFFER_H

#include <kernel/types.h>
#include <kernel/multiboot2.h>

/* 常用 32 位 ARGB/XRGB 颜色 */
#define FB_BLACK   0x00000000
#define FB_WHITE   0x00FFFFFF
#define FB_GRAY    0x00AAAAAA
#define FB_RED     0x00FF5555
#define FB_GREEN   0x0050FA7B
#define FB_BLUE    0x00579BFE
#define FB_YELLOW  0x00F1FA8C
#define FB_PINK    0x00FF79C6
#define FB_CYAN    0x008BE9FD
#define FB_BG      0x00101828   /* 深色背景 */

/* 初始化，返回 true 表示帧缓冲可用 */
bool     fb_init(const boot_info_t *bi);
bool     fb_available(void);
uint32_t fb_width(void);
uint32_t fb_height(void);

/* 基础绘制 */
void fb_clear(uint32_t color);
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
/* 按 Unicode 码点 cp 渲染一个字符（ASCII 或 CJK），字形由 fb_get_glyph 解析 */
void fb_draw_char(uint32_t px, uint32_t py, uint32_t cp, uint32_t fg, uint32_t bg);

/* 图形文本控制台 */
void fbcon_init(void);
void fbcon_set_color(uint32_t fg, uint32_t bg);
void fbcon_putc(char c);
void fbcon_write(const char *s);

#endif /* _SUKI_KERNEL_FRAMEBUFFER_H */
