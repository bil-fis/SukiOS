/*
 * include/kernel/display_cfg.h
 * -----------------------------------------------------------------------------
 * 显示服务【运行时配置】解析结果（来自 GRUB 经 module2 加载的 configs/display.cfg）。
 *
 * 与 include/kernel/config.h（编译期 SMP 等开关）不同，本文件描述的是：
 *   内核在 fb_init 之前，从 boot_info_t.cfg_*（引导模块物理区间）解析出的、
 *   决定显示模式与逻辑分辨率的运行时参数。默认值为 1280x720、视频模式开启，
 *   即使完全没有加载配置文件也成立（保证「无配置也能跑」的健壮性）。
 *
 * 字段语义：
 *   video_mode : true  => 启用帧缓冲视频模式（display_server 合成图形桌面）
 *                false => 禁用帧缓冲，内核回退 VGA 文本模式（纯文本输出）
 *   width/height: display_server 声明的逻辑画布尺寸（像素）。硬件帧缓冲实际
 *                 分辨率由 QEMU -vga std + GRUB gfxpayload 决定；此值用于合成
 *                 画布缩放/标注，并写入启动日志以供验证。
 * ---------------------------------------------------------------------------
 */
#ifndef _SUKI_KERNEL_DISPLAY_CFG_H
#define _SUKI_KERNEL_DISPLAY_CFG_H

#include <kernel/types.h>

/* 默认显示参数（无配置文件时采用） */
#define DISPLAY_DEFAULT_WIDTH  1280
#define DISPLAY_DEFAULT_HEIGHT 720

/* 解析后的全局显示配置（kernel/display_cfg.c 定义，framebuffer.c / 显示服务
 * 经 SYS_FRAMEBUFFER_MAP 读取）。 */
typedef struct display_config {
    bool    video_mode;       /* true=视频模式, false=纯文本回退 */
    uint32_t width;           /* 逻辑画布宽 */
    uint32_t height;          /* 逻辑画布高 */
    bool    parsed;          /* 是否已成功解析过配置文件 */
} display_config_t;

extern display_config_t g_display;

/*
 * display_cfg_parse —— 从引导模块物理区间解析显示配置。
 *   cfg_phys : 配置文本物理地址（0=无模块）；cfg_size : 字节数（含 NUL）
 * 解析失败或缺失时保持默认值（video_mode=true, 1280x720），绝不破坏启动。
 * 仅认领首个 module2 模块为 display.cfg（仓库约定 configs/display.cfg）。
 */
void display_cfg_parse(uint64_t cfg_phys, uint32_t cfg_size);

#endif /* _SUKI_KERNEL_DISPLAY_CFG_H */
