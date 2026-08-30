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

/* 帧缓冲运行时状态（fb_init 后填充；SYS_FRAMEBUFFER_MAP 读取供显示服务映射）。
 * 非 static 导出，供 syscall 层映射帧缓冲物理页到 Ring3 显示服务地址空间。 */
typedef struct fb_info {
    volatile uint8_t *base;   /* 显存内核虚拟基址（线性映射 = phys + 0xFFFF800000000000） */
    uint32_t pitch;           /* 每行字节数（stride） */
    uint32_t width;           /* 像素宽 */
    uint32_t height;          /* 像素高 */
    uint8_t  bpp;             /* 每像素位数（当前固定 32） */
    bool     ready;           /* 帧缓冲是否初始化可用 */
} fb_info_t;

extern fb_info_t g_fb;        /* 全局帧缓冲状态（framebuffer.c 定义） */

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

/*
 * fb_map_result_t —— SYS_FRAMEBUFFER_MAP 的返回结构（内核 -> 用户态 copy_to_user）。
 * 显示服务（Ring3）据此获得可直接写入的帧缓冲用户虚拟地址，以及实际/逻辑分辨率。
 *   enabled    : 1=视频模式可用（已映射 FB），0=纯文本模式（fb_user_va 无效）
 *   fb_user_va : 用户态可直接写像素的虚拟地址（enabled=1 时有效）
 *   fb_phys    : 帧缓冲物理地址（诊断用）
 *   pitch      : 每行字节数（stride）
 *   width/height: 实际硬件帧缓冲分辨率（像素）
 *   bpp        : 每像素位数（当前固定 32）
 *   cfg_width/cfg_height: 配置文件声明的逻辑画布尺寸（来自 display.cfg）
 */
typedef struct fb_map_result {
    uint32_t enabled;
    uint64_t fb_user_va;
    uint64_t fb_phys;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t cfg_width;
    uint32_t cfg_height;
} fb_map_result_t;

#endif /* _SUKI_KERNEL_FRAMEBUFFER_H */
