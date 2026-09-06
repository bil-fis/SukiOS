/*
 * user/lib/suki_gui.h
 * -----------------------------------------------------------------------------
 * SukiOS 用户态 GUI 客户端库（libsuki_gui）公共 API。
 *
 * 设计定位：参考 Win32 原生接口风格的最小可用子集——
 *   - 窗口对象经 suki_create_window 创建（类 CreateWindowEx），返回句柄；
 *   - 绘制前取离屏缓冲（类 GetDC/BeginPaint），用像素/Draw* 原语绘制；
 *   - suki_flush 提交脏矩形（类 EndPaint / Present），经 OOL 零拷贝交给 WM 合成；
 *   - 事件经 suki_poll_event 拉取（类 GetMessage/PeekMessage 的消息循环）。
 *
 * 像素格式：xRGB32（0x00RRGGBB，每像素 4 字节）。
 * 离屏缓冲由本库用 sys_mmap 分配（应用可直写像素，亦可经绘制原语）。
 */
#ifndef _SUKI_GUI_H
#define _SUKI_GUI_H

#include <stdint.h>
#include <stdbool.h>
#include "lib/gui_ipc.h"

typedef struct suki_window suki_window_t;
struct suki_window {
    suki_window_id_t id;
    int32_t  x, y;
    uint32_t w, h;
    uint32_t style;
    void    *buffer;        /* 应用本地离屏缓冲（sys_mmap，可直接像素访问） */
    uint32_t buffer_size;   /* 字节数 = w*h*4 */
    uint32_t event_port;    /* 0 = 未注册事件端口 */
    char     title[64];
};

/* ---- 窗口生命周期 ---- */
suki_window_t *suki_create_window(const char *title, int x, int y,
                                  uint32_t w, uint32_t h, uint32_t style);
void           suki_destroy_window(suki_window_t *w);

/* ---- 缓冲与绘制（xRGB32） ---- */
void *suki_get_buffer(suki_window_t *w);
void  suki_set_pixel(suki_window_t *w, int x, int y, uint32_t rgb);
void  suki_fill_rect(suki_window_t *w, int x, int y, uint32_t ww, uint32_t hh, uint32_t rgb);
void  suki_draw_line(suki_window_t *w, int x0, int y0, int x1, int y1, uint32_t rgb);
void  suki_draw_text(suki_window_t *w, int x, int y, const char *text, uint32_t rgb);
/* 提交脏矩形到 WM（OOL 零拷贝）。x,y,ww,hh 为相对窗口的客户区坐标。 */
void  suki_flush(suki_window_t *w, int x, int y, uint32_t ww, uint32_t hh);

/* ---- 事件 ---- */
void suki_set_event_port(suki_window_t *w, uint32_t port);  /* 注册事件端口后 WM 主动推送 */
bool suki_poll_event(suki_window_t *w, suki_event_t *out);  /* 非阻塞，无事件返回 false */

/* ---- 颜色辅助 ---- */
#define SUKI_RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define SUKI_BG_DESKTOP   SUKI_RGB(16, 25, 38)
#define SUKI_BG_WINDOW    SUKI_RGB(20, 20, 28)
#define SUKI_FG_WINDOW    SUKI_RGB(224, 224, 232)
#define SUKI_ACCENT       SUKI_RGB(0x57, 0x9B, 0xFE)

#endif /* _SUKI_GUI_H */
