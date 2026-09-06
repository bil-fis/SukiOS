/*
 * user/lib/gui_ipc.h
 * -----------------------------------------------------------------------------
 * SukiOS 窗口系统 IPC 协议（应用 [libsuki_gui] <-> 窗口管理器 [display_server/WM]）。
 *
 * 设计依据（实际内核能力）：
 *   - Mach 端口：WM 认领 WM_PORT；应用经 WM_PORT 发请求，WM 经请求头中的
 *     msgh_local_port（应用临时分配的应答端口）回送应答（同步调用模式）。
 *   - OOL 零拷贝：窗口离屏缓冲由应用 sys_mmap 分配（页对齐、≤16 页=64KiB），
 *     提交时 msgh_bits 置 MACH_MSGH_BITS_OOL，ool_desc_t 紧跟消息头，内核把
 *     应用物理页重映射到 WM 地址空间（引用计数+1），WM 拷入自有保留缓冲后
 *     经 mach_msg_destroy 解映射（见 kernel/ipc/port.c）。
 *
 * 布局红线：OOL 描述符必须紧跟 mach_msg_header_t（内核 ool_capture 从
 * header 之后读取 ool_desc_t），故所有携带 OOL 的消息把 ool 字段放在 header 后。
 */
#ifndef _SUKI_GUI_IPC_H
#define _SUKI_GUI_IPC_H

#include <stdint.h>
#include <stdbool.h>
#include "lib/suki.h"

/* 窗口管理器端口（须与 include/ipc/port.h 的 WM_PORT 一致） */
#define WM_PORT 14

/* ---- 消息 id ---- */
/* 应用 -> WM_PORT */
#define WM_MSG_CREATE    0x300   /* 创建窗口（同步应答 WM_MSG_CREATE_RESP） */
#define WM_MSG_FLUSH     0x301   /* 提交离屏缓冲（OOL，零拷贝） */
#define WM_MSG_DESTROY   0x302   /* 销毁窗口 */
#define WM_MSG_SET_EVENT  0x303   /* 设置窗口事件端口 */
#define WM_MSG_SET_TITLE  0x304   /* 更新窗口标题 */
/* WM -> 应用（应答端口） */
#define WM_MSG_CREATE_RESP 0x380
/* WM -> 应用事件端口（窗口注册的事件端口） */
#define WM_MSG_EVENT      0x400

/* ---- 窗口风格标志（参考 Win32 WS_*） ---- */
#define SUKI_WS_VISIBLE  0x0001
#define SUKI_WS_BORDER   0x0002
#define SUKI_WS_TITLEBAR 0x0004
#define SUKI_WS_RESIZE   0x0008
#define SUKI_WS_DEFAULT  (SUKI_WS_VISIBLE | SUKI_WS_BORDER | SUKI_WS_TITLEBAR)

typedef uint32_t suki_window_id_t;
#define SUKI_WINDOW_ID_INVALID ((suki_window_id_t)-1)

/* 创建请求 */
typedef struct wm_create_req {
    mach_msg_header_t h;
    int32_t  x, y;
    uint32_t w, height;
    uint32_t style;
    uint32_t event_port;       /* 应用接收事件的端口（0=不需要） */
    char     title[64];
} wm_create_req_t;

/* 创建应答（WM 经请求的 msgh_local_port 回送） */
typedef struct wm_create_resp {
    mach_msg_header_t h;
    suki_window_id_t  id;
    uint32_t          status;  /* 0=成功 */
} wm_create_resp_t;

/* 提交离屏缓冲：OOL 描述符紧跟头，内核据此捕获应用物理页 */
typedef struct wm_flush_req {
    mach_msg_header_t h;
    ool_desc_t        ool;     /* 必须紧跟 header */
    suki_window_id_t  id;
    uint32_t          x, y, w, height;  /* 脏矩形（相对窗口） */
} wm_flush_req_t;

typedef struct wm_destroy_req {
    mach_msg_header_t h;
    suki_window_id_t  id;
} wm_destroy_req_t;

typedef struct wm_set_event_req {
    mach_msg_header_t h;
    suki_window_id_t  id;
    uint32_t          event_port;
} wm_set_event_req_t;

/* ---- 事件（WM -> 应用） ---- */
typedef enum suki_event_type {
    SUKI_EVENT_KEY_DOWN = 1,
    SUKI_EVENT_KEY_UP,
    SUKI_EVENT_MOUSE_MOVE,
    SUKI_EVENT_MOUSE_DOWN,
    SUKI_EVENT_MOUSE_UP,
    SUKI_EVENT_MOUSE_WHEEL,
    SUKI_EVENT_WINDOW_CLOSE,
    SUKI_EVENT_WINDOW_RESIZE,
    SUKI_EVENT_WINDOW_FOCUS,
} suki_event_type_t;

typedef struct suki_event {
    uint32_t type;
    uint64_t timestamp;
    union {
        struct { uint32_t keycode; uint32_t modifiers; } key;
        struct { int32_t x, y; uint32_t buttons; } mouse;  /* 相对窗口坐标 */
    } u;
} suki_event_t;

typedef struct wm_event_msg {
    mach_msg_header_t h;
    suki_window_id_t  window_id;
    suki_event_t      event;
} wm_event_msg_t;

#endif /* _SUKI_GUI_IPC_H */
