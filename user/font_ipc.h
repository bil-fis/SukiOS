/*
 * user/font_ipc.h
 * -----------------------------------------------------------------------------
 * 字体服务（fontsrv）与客户端（pchfnt）之间的 Mach 消息协议。
 *
 * 端口：FONT_PORT（10，见 suki.h / include/ipc/port.h）
 *
 * 消息流：
 *   客户端 -> FONT_PORT : FONT_MSG_RENDER（内联负载 font_render_req_t）
 *   FONT_PORT -> 客户端(msgh_local_port) : FONT_MSG_RENDER_DONE（回执）
 *
 * 字体服务常驻，首次按 font_path 加载 TTF（FT_Open_Face 内存加载）并缓存
 * FT_Face；渲染时按 (face, glyph, pixel_size) 做字形缓存（LRU），
 * 详见 OSDev Font 相关条目（字形缓存可避免重复光栅化开销）。
 */
#ifndef _SUKI_FONT_IPC_H
#define _SUKI_FONT_IPC_H

#include <stdint.h>

#define FONT_MSG_RENDER       200   /* 客户端请求渲染文本 */
#define FONT_MSG_RENDER_DONE  201   /* 字体服务回执（成功/失败 + 统计） */

/* 文本最大长度（内联消息承载，超出用 OOL 以后再做） */
#define FONT_TEXT_MAX   1024

/* 像素格式：RGBA8888（与 display_server 帧缓冲一致时可直接 BLIT） */
typedef struct font_render_req {
    mach_msg_header_t h;
    uint32_t req_id;            /* 请求序号，回执带回 */
    uint32_t pixel_size;        /* 字号（像素高度） */
    int32_t  x;                 /* 屏幕起点 X */
    int32_t  y;                 /* 屏幕起点 Y */
    uint32_t color;             /* 0xRRGGBB 文本颜色（alpha 固定 0xFF） */
    uint32_t unicode_mode;      /* 0=UTF-8 文本；1=后面 text[] 是空格分隔的 Unicode 码点(十进制/十六进制) */
    char     font_path[256];    /* 字体文件路径（如 ::FONTS/RESOURCEHANROUNDEDCN-MEDIUM.TTF） */
    char     text[FONT_TEXT_MAX]; /* 待渲染文本（UTF-8 或 Unicode 码点串） */
} font_render_req_t;

typedef struct font_render_done {
    mach_msg_header_t h;
    uint32_t req_id;
    int32_t  status;            /* 0=成功；<0=失败 */
    int32_t  glyphs;            /* 实际渲染字形数 */
    int32_t  cache_hits;        /* 字形缓存命中数 */
    int32_t  bbox_w;            /* 渲染后整体包围盒宽 */
    int32_t  bbox_h;
} font_render_done_t;

#endif /* _SUKI_FONT_IPC_H */
