/*
 * user/display_server.c
 * -----------------------------------------------------------------------------
 * Ring3 显示服务 / 窗口管理器 / 纯合成器（SukiOS 图形栈核心）。
 *
 * 职责（单一帧缓冲拥有者）：
 *   1) 合成器（compositor）：维护窗口注册表（Z-order = 创建顺序），每条窗口持有
 *      自有保留缓冲 win->pixels（由本服务 sys_mmap 分配）。应用经 WM_PORT 提交离屏
 *      帧时，本服务通过 OOL 零拷贝把应用的物理页映射到自身地址空间，拷入
 *      win->pixels，再解映射；composite() 按 Z-order 把各窗口像素 blit 到帧缓冲，
 *      并叠加边框/标题栏（纯几何）/关闭按钮/光标。
 *   2) 窗口管理器（WM）：创建/销毁/置焦/拖拽/关闭、事件端口路由。
 *   3) 输入分发：鼠标事件经 DISPLAY_PORT 到达（mouse_server 推送），本服务做命中
 *      测试、更新焦点，并把事件经窗口注册的事件端口推送给对应应用。键盘事件同样
 *      转发给当前焦点窗口。
 *
 * 关键设计（用户需求）：本服务【绝不渲染任何字符】。所有文本/图形内容都由各窗口
 * 应用自行绘制进自己的离屏缓冲（例如 shell 的终端仿真、winhello 的自绘），再以图像
 * 形式经 OOL 提交给本服务合成。内核 console / 启动日志 / 用户态 print 不再被本服务
 * 捕获渲染——它们只走串口，由相关应用自行决定是否在其窗口内显示。
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include "lib/suki.h"
#include "lib/gui_ipc.h"

/* 鼠标事件消息 id（与 user/mouse_server.c 保持一致） */
#define MOUSE_MSG_MOVE   101
#define MOUSE_MSG_BUTTON 102
#define MOUSE_MSG_WHEEL  103
#define MOUSE_CURSOR_W 12
#define MOUSE_CURSOR_H 18

/* ---- 帧缓冲映射结果（与内核 include/kernel/framebuffer.h 逐字节布局一致） ---- */
typedef struct fb_map_result {
    uint32_t enabled;
    uint32_t _pad0;
    uint64_t fb_user_va;
    uint64_t fb_phys;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t cfg_width;
    uint32_t cfg_height;
} fb_map_result_t;

/* ---- 帧缓冲与离屏背景缓冲 ---- */
static volatile uint32_t *g_fb = NULL;   /* 真实帧缓冲（xRGB32） */
static uint32_t *g_desk = NULL;          /* 离屏背景层（合成源，仅纯色背景） */
static uint32_t g_fb_width  = 0;
static uint32_t g_fb_height = 0;
static uint32_t g_fb_pitch  = 0;         /* 字节/行 */
static uint32_t g_stride    = 0;         /* 像素/行 = pitch/4 */

/* 桌面顶部装饰条高度（纯几何，无文字） */
#define TITLE_H    40

/* 颜色（xRGB32） */
#define COL_DESKTOP   0x00101926
#define COL_BAR       0x003A2A4A
#define COL_ACCENT    0x00579BFE
#define COL_CLOSEBG   0x00C0392B
#define COL_CLOSEFG   0x00FFFFFF

/* 像素写入真实帧缓冲（含边界保护） */
static inline void fb_px(uint32_t x, uint32_t y, uint32_t rgb)
{
    if (x >= g_fb_width || y >= g_fb_height) return;
    g_fb[y * g_stride + x] = rgb;
}

static void fill_rect_fb(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb)
{
    for (uint32_t j = 0; j < h; j++)
        for (uint32_t i = 0; i < w; i++) {
            uint32_t px = x + i, py = y + j;
            if (px >= g_fb_width || py >= g_fb_height) continue;
            g_fb[py * g_stride + px] = rgb;
        }
}

/* 仅绘制纯色背景 + 顶部装饰条（不渲染任何字符 / 终端窗口） */
static void draw_desktop(void)
{
    for (uint32_t y = 0; y < g_fb_height; y++)
        for (uint32_t x = 0; x < g_fb_width; x++)
            g_desk[y * g_stride + x] = COL_DESKTOP;
    fill_rect_fb(0, 0, g_fb_width, TITLE_H, COL_BAR);
}

/* ===========================================================================
 * 窗口管理器（WM）部分
 * ======================================================================== */
#define WM_MAX_WINDOWS 64

typedef struct wm_window {
    suki_window_id_t id;
    int32_t  x, y;
    uint32_t w, h;
    uint32_t style;
    uint32_t z;             /* 越大越靠上（数组顺序即 Z，后续窗口在上） */
    uint32_t event_port;    /* 应用事件端口（0=无） */
    bool     visible;
    bool     has_focus;
    uint32_t *pixels;       /* 本服务自有保留缓冲（合成时读取） */
    char     title[64];
} wm_window_t;

static wm_window_t g_wins[WM_MAX_WINDOWS];
static int g_win_count = 0;
static suki_window_id_t g_win_next_id = 1;

/* 窗口标题栏高度（与 composite 绘制保持一致） */
#define WIN_TITLE_H 20

/* 焦点窗口与拖拽状态 */
static wm_window_t *g_focus     = NULL;
static wm_window_t *g_drag      = NULL;
static int32_t      g_drag_offx = 0, g_drag_offy = 0;

static wm_window_t *wm_find(suki_window_id_t id)
{
    for (int i = 0; i < g_win_count; i++)
        if (g_wins[i].id == id) return &g_wins[i];
    return NULL;
}

/* 前向声明 */
static void wm_set_focus(wm_window_t *w);
static void composite(void);

/* 命中测试：返回光标下最上层（最后绘制）的可见窗口 */
static wm_window_t *wm_hit_test(int32_t sx, int32_t sy)
{
    wm_window_t *hit = NULL;
    for (int i = 0; i < g_win_count; i++) {
        wm_window_t *w = &g_wins[i];
        if (!w->visible) continue;
        if (sx >= w->x && sx < (int32_t)(w->x + w->w) &&
            sy >= w->y && sy < (int32_t)(w->y + w->h))
            hit = w;
    }
    return hit;
}

static void wm_handle_create(const wm_create_req_t *req, mach_msg_header_t *hdr)
{
    wm_create_resp_t resp; memset(&resp, 0, sizeof(resp));
    if (g_win_count >= WM_MAX_WINDOWS) goto fail;
    uint64_t need = (uint64_t)req->w * req->height * 4;
    if (need == 0 || need > 16 * 4096) goto fail;
    uint32_t *px = (uint32_t *)sys_mmap(need, 3);
    if (!px) goto fail;

    wm_window_t *w = &g_wins[g_win_count++];
    w->id = g_win_next_id++;
    w->x = req->x; w->y = req->y; w->w = req->w; w->h = req->height;
    w->style = req->style; w->z = (uint32_t)g_win_count;
    w->event_port = req->event_port; w->visible = true; w->has_focus = false;
    w->pixels = px;
    for (uint32_t i = 0; i < sizeof(w->title) - 1 && req->title[i]; i++)
        w->title[i] = req->title[i];
    w->title[sizeof(w->title) - 1] = '\0';

    resp.h.msgh_bits        = MACH_SEND_MSG;
    resp.h.msgh_size        = sizeof(resp);
    resp.h.msgh_remote_port = hdr->msgh_local_port;
    resp.h.msgh_local_port  = 0;
    resp.h.msgh_id          = WM_MSG_CREATE_RESP;
    resp.id = w->id; resp.status = 0;
    mach_msg_send(&resp, sizeof(resp));
    u_print("[wm] window created id=");
    char b[16]; u_print(u_utoa_s(w->id, b, sizeof(b)));
    u_print("\n");
    wm_set_focus(w);
    composite();
    return;
fail:
    resp.h.msgh_bits        = MACH_SEND_MSG;
    resp.h.msgh_size        = sizeof(resp);
    resp.h.msgh_remote_port = hdr->msgh_local_port;
    resp.h.msgh_local_port  = 0;
    resp.h.msgh_id          = WM_MSG_CREATE_RESP;
    resp.id = SUKI_WINDOW_ID_INVALID; resp.status = (uint32_t)-1;
    mach_msg_send(&resp, sizeof(resp));
}

static void wm_handle_flush(wm_flush_req_t *f)
{
    wm_window_t *w = wm_find(f->id);
    if (!w) return;
    uint32_t *src = (uint32_t *)(uintptr_t)f->ool.address;
    uint64_t cp = (uint64_t)w->w * w->h * 4;
    if (f->ool.size < cp) cp = f->ool.size;
    memcpy(w->pixels, src, cp);
    /* 解映射收到的 OOL 物理页（引用计数 -1）；mach_msg_destroy 仅取 OOL 虚拟地址 */
    mach_msg_destroy((uint64_t)f->ool.address);
    composite();
}

static void wm_handle_destroy(const wm_destroy_req_t *m)
{
    u_print("[wm] destroy recv id=");
    char b[16]; u_print(u_utoa_s(m->id, b, sizeof(b)));
    u_print("\n");
    wm_window_t *w = wm_find(m->id);
    if (!w) return;
    /* 销毁前清理焦点/拖拽悬空引用，避免键盘/拖拽后续转发到已释放窗口 */
    if (g_focus == w) g_focus = NULL;
    if (g_drag  == w) g_drag  = NULL;
    uint64_t need = (uint64_t)w->w * w->h * 4;
    sys_munmap(w->pixels, need);
    /* 从数组中移除（保留顺序=Z 稳定） */
    int idx = (int)(w - g_wins);
    for (int i = idx; i + 1 < g_win_count; i++) g_wins[i] = g_wins[i + 1];
    g_win_count--;
    u_print("[wm] window destroyed\n");
    composite();
}

static void wm_handle_set_event(const wm_set_event_req_t *m)
{
    wm_window_t *w = wm_find(m->id);
    if (w) w->event_port = m->event_port;
}

/* 把事件推送给窗口注册的应用事件端口 */
static void wm_forward_event(wm_window_t *w, suki_event_t *ev)
{
    if (!w || w->event_port == 0) return;
    wm_event_msg_t m; memset(&m, 0, sizeof(m));
    m.h.msgh_bits        = MACH_SEND_MSG;
    m.h.msgh_size        = sizeof(m);
    m.h.msgh_remote_port = w->event_port;
    m.h.msgh_local_port  = 0;
    m.h.msgh_id          = WM_MSG_EVENT;
    m.window_id = w->id;
    m.event = *ev;
    mach_msg_send(&m, sizeof(m));
}

/* 关闭按钮命中（标题栏右上角 16x16 区域） */
static bool wm_in_close_box(const wm_window_t *w, int32_t sx, int32_t sy)
{
    if (!(w->style & SUKI_WS_TITLEBAR)) return false;
    int32_t bx = w->x + (int32_t)w->w - 18;
    int32_t by = w->y + 2;
    return (sx >= bx && sx < bx + 16 && sy >= by && sy < by + 16);
}

/* 设置焦点窗口并向相关窗口广播 SUKI_EVENT_WINDOW_FOCUS（buttons:1=得焦点,0=失焦点） */
static void wm_set_focus(wm_window_t *w)
{
    if (g_focus == w) return;
    if (g_focus) {
        g_focus->has_focus = false;
        suki_event_t ev; memset(&ev, 0, sizeof(ev));
        ev.type = SUKI_EVENT_WINDOW_FOCUS;
        ev.u.mouse.buttons = 0;
        wm_forward_event(g_focus, &ev);
    }
    g_focus = w;
    if (g_focus) {
        g_focus->has_focus = true;
        suki_event_t ev; memset(&ev, 0, sizeof(ev));
        ev.type = SUKI_EVENT_WINDOW_FOCUS;
        ev.u.mouse.buttons = 1;
        wm_forward_event(g_focus, &ev);
    }
}

/* ===========================================================================
 * 合成器
 * ======================================================================== */
/* 12x18 光标位图：1=前景(白) */
static const uint8_t g_cursor_mask[MOUSE_CURSOR_H][MOUSE_CURSOR_W] = {
    {1,0,0,0,0,0,0,0,0,0,0,0}, {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,0,0,0,0,0,0,0,0,0}, {1,1,1,1,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,0,0,0,0,0,0,0}, {1,1,1,1,1,1,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,0,0,0,0,0}, {1,1,1,1,1,1,1,1,0,0,0,0},
    {1,1,1,1,1,1,1,1,1,0,0,0}, {1,1,1,1,1,1,1,1,1,1,0,0},
    {1,1,1,1,1,1,1,1,1,1,1,0}, {1,1,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,1,1,1,1,1,1,1,0,0}, {1,1,1,1,1,1,1,1,0,0,0,0},
    {1,1,1,1,1,1,0,0,0,0,0,0}, {1,1,1,1,1,0,0,0,0,0,0,0},
    {1,1,1,0,0,0,0,0,0,0,0,0}, {1,1,0,0,0,0,0,0,0,0,0,0},
};

static int32_t g_cur_x = 0;
static int32_t g_cur_y = 0;
static uint32_t g_cur_buttons = 0;

static void draw_cursor_on_fb(void)
{
    for (int32_t j = 0; j < MOUSE_CURSOR_H; j++)
        for (int32_t i = 0; i < MOUSE_CURSOR_W; i++) {
            int32_t x = g_cur_x + i, y = g_cur_y + j;
            if (x < 0 || y < 0 || (uint32_t)x >= g_fb_width || (uint32_t)y >= g_fb_height) continue;
            if (g_cursor_mask[j][i])
                g_fb[y * g_stride + x] = (g_cur_buttons & 1) ? 0x00FF3030 : 0x00FFFFFF;
        }
}

static void composite(void)
{
    if (!g_fb || !g_desk) return;
    /* 1) 背景层整体 blit */
    memcpy((void *)g_fb, g_desk, (size_t)g_fb_height * g_stride * sizeof(uint32_t));
    /* 2) 窗口（Z-order = 创建顺序，后者在上） */
    for (int i = 0; i < g_win_count; i++) {
        wm_window_t *w = &g_wins[i];
        if (!w->visible) continue;
        int32_t x0 = w->x, y0 = w->y;
        uint32_t ww = w->w, hh = w->h;
        if (x0 + (int32_t)ww <= 0 || y0 + (int32_t)hh <= 0 ||
            x0 >= (int32_t)g_fb_width || y0 >= (int32_t)g_fb_height) continue;
        int32_t cx0 = x0 < 0 ? 0 : x0;
        int32_t cy0 = y0 < 0 ? 0 : y0;
        int32_t cx1 = (x0 + (int32_t)ww > (int32_t)g_fb_width) ? (int32_t)g_fb_width : x0 + (int32_t)ww;
        int32_t cy1 = (y0 + (int32_t)hh > (int32_t)g_fb_height) ? (int32_t)g_fb_height : y0 + (int32_t)hh;
        for (int32_t y = cy0; y < cy1; y++)
            for (int32_t x = cx0; x < cx1; x++)
                g_fb[y * g_stride + x] = w->pixels[(y - y0) * ww + (x - x0)];
        /* 窗口装饰：边框 + 标题栏（纯几何，无文字）+ 关闭按钮 */
        uint32_t border = 2;
        fill_rect_fb((uint32_t)x0, (uint32_t)y0, ww, border, COL_ACCENT);
        fill_rect_fb((uint32_t)x0, (uint32_t)y0 + hh - border, ww, border, COL_ACCENT);
        fill_rect_fb((uint32_t)x0, (uint32_t)y0, border, hh, COL_ACCENT);
        fill_rect_fb((uint32_t)x0 + ww - border, (uint32_t)y0, border, hh, COL_ACCENT);
        if (w->style & SUKI_WS_TITLEBAR) {
            uint32_t bar_h = WIN_TITLE_H;
            if (hh >= bar_h) {
                fill_rect_fb((uint32_t)x0 + border, (uint32_t)y0 + border,
                             ww - border * 2, bar_h - border,
                             w->has_focus ? COL_ACCENT : COL_BAR);
                /* 关闭按钮（标题栏右上角）：红底 + 白色 X（纯几何，非字符渲染） */
                int32_t bx = (int32_t)x0 + (int32_t)ww - 18;
                int32_t by = (int32_t)y0 + 2;
                if (bx >= 0 && by >= 0) {
                    fill_rect_fb((uint32_t)bx, (uint32_t)by, 16, 16, COL_CLOSEBG);
                    for (int32_t i = 3; i < 13; i++) {
                        if (bx + i < (int32_t)g_fb_width && by + i < (int32_t)g_fb_height)
                            g_fb[(by + i) * g_stride + (bx + i)] = COL_CLOSEFG;
                        if (bx + (15 - i) >= 0 && bx + (15 - i) < (int32_t)g_fb_width &&
                            by + i < (int32_t)g_fb_height)
                            g_fb[(by + i) * g_stride + (bx + (15 - i))] = COL_CLOSEFG;
                    }
                }
            }
        }
    }
    /* 3) 光标（最上层） */
    draw_cursor_on_fb();
}

int main(void)
{
    /* 1) 认领显示端口与窗口管理器端口 */
    if (sys_port_claim(DISPLAY_PORT) != 0) { u_print("display: DISPLAY_PORT claim failed\n"); sys_exit(1); }
    if (sys_port_claim(WM_PORT) != 0)        { u_print("display: WM_PORT claim failed\n");     sys_exit(1); }

    /* 2) 取得帧缓冲用户态映射 */
    fb_map_result_t res;
    if (suki_syscall2(SYS_FRAMEBUFFER_MAP, (uint64_t)&res, 0) != 0 || !res.enabled) {
        u_print("display: framebuffer map failed\n");
        sys_exit(1);
    }
    g_fb        = (volatile uint32_t *)res.fb_user_va;
    g_fb_width  = res.width;
    g_fb_height = res.height;
    g_fb_pitch  = res.pitch;
    g_stride    = res.pitch / 4;

    /* 离屏背景缓冲（与帧缓冲同尺寸） */
    g_desk = (uint32_t *)sys_mmap((uint64_t)g_fb_height * g_stride * sizeof(uint32_t), 3);
    if (!g_desk) { u_print("display: desk alloc failed\n"); sys_exit(1); }

    u_print("display: fb mapped @32bpp, pure compositor ready\n");

    /* 3) 绘制背景层 */
    draw_desktop();

    /* 4) 通知内核：显示服务已接管帧缓冲（纯合成器，不再渲染字符） */
    suki_syscall1(SYS_DISPLAY_READY, 0);

    /* 5) 消息循环：同时轮询 DISPLAY_PORT（鼠标/键盘）与 WM_PORT（窗口管理） */
    static uint8_t msgbuf[512];
    typedef struct { mach_msg_header_t h; int32_t x; int32_t y; uint8_t buttons; int8_t wheel; uint8_t _pad[3]; } mouse_event_msg_t;

    for (;;) {
        /* 显示端口：鼠标与键盘事件 */
        if (mach_msg_tryrecv(msgbuf, sizeof(msgbuf), DISPLAY_PORT) == 0) {
            mach_msg_header_t *h = (mach_msg_header_t *)msgbuf;
            if (h->msgh_id == MOUSE_MSG_MOVE ||
                h->msgh_id == MOUSE_MSG_BUTTON ||
                h->msgh_id == MOUSE_MSG_WHEEL) {
                mouse_event_msg_t *m = (mouse_event_msg_t *)msgbuf;
                g_cur_x = m->x; g_cur_y = m->y;
                if (h->msgh_id == MOUSE_MSG_BUTTON) g_cur_buttons = m->buttons;

                /* 拖拽进行中：直接跟随光标移动窗口（忽略命中） */
                if (g_drag) {
                    if (h->msgh_id == MOUSE_MSG_MOVE) {
                        g_drag->x = m->x - g_drag_offx;
                        g_drag->y = m->y - g_drag_offy;
                        composite();
                    }
                    continue;
                }

                wm_window_t *hit = wm_hit_test(m->x, m->y);

                if (h->msgh_id == MOUSE_MSG_BUTTON && (m->buttons & 1)) {
                    /* 左键按下：标题栏 -> 关闭按钮 / 拖拽；客户区 -> 聚焦 + 下发 */
                    if (hit && (hit->style & SUKI_WS_TITLEBAR)) {
                        if (wm_in_close_box(hit, m->x, m->y)) {
                            suki_event_t ev; memset(&ev, 0, sizeof(ev));
                            ev.type = SUKI_EVENT_WINDOW_CLOSE;
                            wm_forward_event(hit, &ev);
                            composite();
                            continue;
                        }
                        if (m->y >= hit->y && m->y < hit->y + (int32_t)WIN_TITLE_H) {
                            g_drag = hit;
                            g_drag_offx = m->x - hit->x;
                            g_drag_offy = m->y - hit->y;
                            wm_set_focus(hit);
                            composite();
                            continue;
                        }
                    }
                    if (hit) wm_set_focus(hit);
                    if (hit) {
                        suki_event_t ev; memset(&ev, 0, sizeof(ev));
                        ev.type = SUKI_EVENT_MOUSE_DOWN;
                        ev.u.mouse.x = m->x - hit->x; ev.u.mouse.y = m->y - hit->y;
                        ev.u.mouse.buttons = m->buttons;
                        wm_forward_event(hit, &ev);
                    }
                    composite();
                } else if (h->msgh_id == MOUSE_MSG_BUTTON) {
                    /* 松开：结束拖拽并下发 MOUSE_UP */
                    g_drag = NULL;
                    if (hit) {
                        suki_event_t ev; memset(&ev, 0, sizeof(ev));
                        ev.type = SUKI_EVENT_MOUSE_UP;
                        ev.u.mouse.x = m->x - hit->x; ev.u.mouse.y = m->y - hit->y;
                        ev.u.mouse.buttons = m->buttons;
                        wm_forward_event(hit, &ev);
                    }
                    composite();
                } else {
                    /* MOVE / WHEEL：转发给命中窗口 */
                    if (hit) {
                        suki_event_t ev; memset(&ev, 0, sizeof(ev));
                        ev.type = (h->msgh_id == MOUSE_MSG_WHEEL)
                                     ? SUKI_EVENT_MOUSE_WHEEL : SUKI_EVENT_MOUSE_MOVE;
                        ev.u.mouse.x = m->x - hit->x; ev.u.mouse.y = m->y - hit->y;
                        ev.u.mouse.buttons = g_cur_buttons;
                        wm_forward_event(hit, &ev);
                    }
                    composite();
                }
            } else if (h->msgh_id == KEY_MSG_DOWN || h->msgh_id == KEY_MSG_UP) {
                /* 键盘事件：转发给当前焦点窗口（由 WM 统一管束输入焦点） */
                key_event_msg_t *k = (key_event_msg_t *)msgbuf;
                if (g_focus && g_focus->event_port) {
                    suki_event_t ev; memset(&ev, 0, sizeof(ev));
                    ev.type = (h->msgh_id == KEY_MSG_DOWN) ? SUKI_EVENT_KEY_DOWN
                                                           : SUKI_EVENT_KEY_UP;
                    ev.u.key.keycode   = k->ascii;
                    ev.u.key.modifiers = k->modifiers;
                    wm_forward_event(g_focus, &ev);
                }
            }
        }

        /* 窗口管理器端口 */
        if (mach_msg_tryrecv(msgbuf, sizeof(msgbuf), WM_PORT) == 0) {
            mach_msg_header_t *h = (mach_msg_header_t *)msgbuf;
            switch (h->msgh_id) {
            case WM_MSG_CREATE:
                wm_handle_create((const wm_create_req_t *)msgbuf, h);
                break;
            case WM_MSG_FLUSH:
                wm_handle_flush((wm_flush_req_t *)msgbuf);
                break;
            case WM_MSG_DESTROY:
                wm_handle_destroy((const wm_destroy_req_t *)msgbuf);
                break;
            case WM_MSG_SET_EVENT:
                wm_handle_set_event((const wm_set_event_req_t *)msgbuf);
                break;
            case WM_MSG_SET_FOCUS: {
                wm_set_focus_req_t *r = (wm_set_focus_req_t *)msgbuf;
                wm_set_focus(wm_find(r->id));
                wm_set_focus_resp_t resp; memset(&resp, 0, sizeof(resp));
                resp.h.msgh_bits        = MACH_SEND_MSG;
                resp.h.msgh_size        = sizeof(resp);
                resp.h.msgh_remote_port = h->msgh_local_port;
                resp.h.msgh_local_port  = 0;
                resp.h.msgh_id          = WM_MSG_SET_FOCUS_RESP;
                resp.status             = 0;
                mach_msg_send(&resp, sizeof(resp));
                break;
            }
            case WM_MSG_SET_POS: {
                wm_set_pos_req_t *r = (wm_set_pos_req_t *)msgbuf;
                wm_window_t *w = wm_find(r->id);
                if (w) { w->x = r->x; w->y = r->y; composite(); }
                break;
            }
            case WM_MSG_GET_FOCUS: {
                wm_get_focus_resp_t resp; memset(&resp, 0, sizeof(resp));
                resp.h.msgh_bits        = MACH_SEND_MSG;
                resp.h.msgh_size        = sizeof(resp);
                resp.h.msgh_remote_port = h->msgh_local_port;
                resp.h.msgh_local_port  = 0;
                resp.h.msgh_id          = WM_MSG_GET_FOCUS_RESP;
                resp.id = g_focus ? g_focus->id : 0;
                mach_msg_send(&resp, sizeof(resp));
                break;
            }
            default:
                break;
            }
        }

        sys_yield();
    }
    return 0;
}
