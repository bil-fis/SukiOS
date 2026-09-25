/*
 * user/font_ipc.h
 * -----------------------------------------------------------------------------
 * 字体服务（fontsrv）与客户端（libsui / pchfnt）之间的 Mach 消息协议。
 *
 * 端口：FONT_PORT（10，见 include/ipc/port.h）
 *
 * 设计变更（v2，离屏渲染）：
 *   旧版 fontsrv 直接把字形 blit 到物理屏幕帧缓冲（自带「合成器」），无法被
 *   libsui 复用。现改为：fontsrv 只负责 FreeType 光栅化 + 把灰度字形按 alpha
 *   混合进「客户端提供的离屏缓冲」，自己不再触碰屏幕。
 *
 * 数据流：
 *   1) 客户端经 FONT_MSG_REGISTER 把自身离屏画布（OOL 物理页）注册给 fontsrv，
 *      fontsrv 持久映射该页并返回 handle；
 *   2) 客户端每需绘制文本，发 FONT_MSG_RENDER_BUF(handle, x,y,size,color,clip,
 *      font,text)；fontsrv 把字形合成进该缓冲（尊重裁剪矩形），回 FONT_MSG_RENDER_DONE；
 *   3) 测量宽度走 FONT_MSG_MEASURE（不渲染，仅返回 advance 像素宽）；
 *   4) 窗口销毁时客户端发 FONT_MSG_UNREGISTER(handle) 让 fontsrv 解映射。
 *
 * 这样 libsui 不再自带 FreeType 合成后端，统一复用 fontsrv；fontsrv 亦不再有
 * 屏幕合成职责（与显示服务 / 合成器解耦）。
 */
#ifndef _SUKI_FONT_IPC_H
#define _SUKI_FONT_IPC_H

#include <stdint.h>
#include "lib/suki.h"

#define FONT_PORT 10

/* ---- 消息 id（客户端 -> FONT_PORT）---- */
#define FONT_MSG_REGISTER     210   /* 注册离屏缓冲（OOL），fontsrv 持久映射，回 handle */
#define FONT_MSG_RENDER_BUF   212   /* 渲染文本到已注册缓冲（按 handle） */
#define FONT_MSG_MEASURE      214   /* 测量文本宽度（不渲染） */
#define FONT_MSG_UNREGISTER   216   /* 解除缓冲映射 */

/* ---- 消息 id（fontsrv -> 客户端，经请求头 msgh_local_port 回执）---- */
#define FONT_MSG_REGISTER_DONE 211
#define FONT_MSG_RENDER_DONE   213
#define FONT_MSG_MEASURE_DONE  215

/* 文本最大长度（内联消息承载） */
#define FONT_TEXT_MAX   1024

/* 像素格式：xRGB32（与显示服务帧缓冲、libsui 画布一致） */
typedef struct font_register_req {
    mach_msg_header_t h;
    ool_desc_t        ool;     /* 必须紧跟 header：注册缓冲的物理页 */
    uint32_t buf_w;
    uint32_t buf_h;
    uint32_t pixel_format;     /* 0 = xRGB32 */
} font_register_req_t;

typedef struct font_register_done {
    mach_msg_header_t h;
    uint32_t status;           /* 0=成功 */
    uint32_t handle;           /* 0=失败 */
} font_register_done_t;

typedef struct font_render_buf_req {
    mach_msg_header_t h;
    uint32_t handle;
    int32_t  x, y;             /* 文本起点（相对缓冲，像素） */
    uint32_t pixel_size;
    uint32_t color;            /* 0xRRGGBB */
    int32_t  clip_x, clip_y, clip_w, clip_h;  /* 裁剪矩形（缓冲局部坐标） */
    uint32_t unicode_mode;     /* 0=UTF-8；1=空格分隔 Unicode 码点 */
    char     font_path[256];
    char     text[FONT_TEXT_MAX];
} font_render_buf_req_t;

typedef struct font_render_done {
    mach_msg_header_t h;
    uint32_t status;
    int32_t  glyphs;
    int32_t  cache_hits;
    int32_t  bbox_w;
    int32_t  bbox_h;
} font_render_done_t;

typedef struct font_measure_req {
    mach_msg_header_t h;
    uint32_t pixel_size;
    uint32_t unicode_mode;
    char     font_path[256];
    char     text[FONT_TEXT_MAX];
} font_measure_req_t;

typedef struct font_measure_done {
    mach_msg_header_t h;
    uint32_t status;
    int32_t  width;            /* advance 像素宽（失败为负） */
} font_measure_done_t;

typedef struct font_unregister_req {
    mach_msg_header_t h;
    uint32_t handle;
} font_unregister_req_t;

#endif /* _SUKI_FONT_IPC_H */
