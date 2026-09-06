/*
 * user/display_server.c
 * -----------------------------------------------------------------------------
 * Ring3 显示服务 / 窗口管理器 / 合成器（SukiOS 图形栈核心）。
 *
 * 三重职责（单一帧缓冲拥有者，最稳）：
 *   1) 桌面 + 终端层：经 SYS_FRAMEBUFFER_MAP 取得帧缓冲用户态线性映射，并在一块
 *      同等大小的【离屏桌面缓冲 g_desk】上绘制桌面背景、标题栏与终端窗口；终端文本
 *      增量绘制到 g_desk，跨帧持久（合成时整体 blit 到帧缓冲，不会擦成字符）。
 *   2) 窗口合成：维护窗口注册表（Z-order = 创建顺序），每条窗口持有自有保留缓冲
 *      （win->pixels，由本服务 sys_mmap 分配）。应用经 WM_PORT 提交离屏帧时，本服务
 *      通过 OOL 零拷贝把应用的物理页映射到自身地址空间，拷入 win->pixels，再解映射；
 *      composite() 按 Z-order 把各窗口像素 blit 到帧缓冲并叠加边框/标题栏，最后绘光标。
 *   3) 输入分发：鼠标事件经 DISPLAY_PORT 到达（mouse_server 推送），本服务做命中测试
 *      （确定光标下窗口）、更新焦点，并把事件经窗口注册的事件端口推送给对应应用。
 *
 * 像素格式：xRGB32（0xRRGGBB，每像素 4 字节）。所有绘制均落在 g_desk，composite()
 * 统一合成到真实帧缓冲 g_fb，避免重绘相互破坏。
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include "lib/suki.h"
#include "lib/font8x8.h"
#include "lib/gui_ipc.h"

/* 显示服务消息 id（与内核 console.c 转发文本消息一致） */
#define DISP_MSG_TEXT 1

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

/* ---- 帧缓冲与离屏桌面缓冲 ---- */
static volatile uint32_t *g_fb = NULL;   /* 真实帧缓冲（xRGB32） */
static uint32_t *g_desk = NULL;          /* 离屏桌面+终端层（合成源） */
static uint32_t g_fb_width  = 0;
static uint32_t g_fb_height = 0;
static uint32_t g_fb_pitch  = 0;         /* 字节/行 */
static uint32_t g_stride    = 0;         /* 像素/行 = pitch/4 */

/* 字形放大倍数：8x8 -> 16x16 */
#define CON_SCALE  2
#define CON_MARGIN 24
#define TITLE_H    40
#define CHAR_W     (8 * CON_SCALE)
#define CHAR_H     (8 * CON_SCALE)

/* 终端窗口客户区文本栅格 */
static uint32_t g_term_x = 0;
static uint32_t g_term_y = 0;
static uint32_t g_term_cols = 0;
static uint32_t g_term_rows = 0;

/* 颜色（xRGB32） */
#define COL_BG        0x002B2B3A
#define COL_DESKTOP   0x00101926
#define COL_BAR       0x003A2A4A
#define COL_WINBG     0x0014141C
#define COL_WINBORDER 0x00606080
#define COL_FG        0x00E0E0E8
#define COL_TITLE     0x00C0C0FF
#define COL_ACCENT    0x00579BFE

/* 像素写入离屏桌面层（含边界保护） */
static inline void desk_px(uint32_t x, uint32_t y, uint32_t rgb)
{
    if (x >= g_fb_width || y >= g_fb_height) return;
    g_desk[y * g_stride + x] = rgb;
}

static void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb)
{
    for (uint32_t j = 0; j < h; j++)
        for (uint32_t i = 0; i < w; i++)
            desk_px(x + i, y + j, rgb);
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

/* 画 8x8 字形的 2x 放大版本到离屏桌面层 */
static void draw_glyph(uint32_t ox, uint32_t oy, uint8_t ch, uint32_t fg)
{
    if (ch >= 128) ch = '?';
    const uint8_t *g = font8x8_basic[ch];
    for (uint32_t row = 0; row < 8; row++) {
        uint8_t bits = g[row];
        for (uint32_t col = 0; col < 8; col++) {
            if (bits & (1u << col)) {
                desk_px(ox + col * CON_SCALE,     oy + row * CON_SCALE,     fg);
                desk_px(ox + col * CON_SCALE + 1, oy + row * CON_SCALE,     fg);
                desk_px(ox + col * CON_SCALE,     oy + row * CON_SCALE + 1, fg);
                desk_px(ox + col * CON_SCALE + 1, oy + row * CON_SCALE + 1, fg);
            }
        }
    }
}

/* 在真实帧缓冲上画 8x8 字形（合成窗口标题栏用） */
static void draw_glyph_fb(uint32_t ox, uint32_t oy, uint8_t ch, uint32_t fg)
{
    if (ch >= 128) ch = '?';
    const uint8_t *g = font8x8_basic[ch];
    for (uint32_t row = 0; row < 8; row++) {
        uint8_t bits = g[row];
        for (uint32_t col = 0; col < 8; col++) {
            if (bits & (1u << col)) {
                uint32_t px = ox + col, py = oy + row;
                if (px >= g_fb_width || py >= g_fb_height) continue;
                g_fb[py * g_stride + px] = fg;
            }
        }
    }
}

/* ---- ANSI/VT100 CSI 转义序列解析（保证 shell 输出不乱码） ---- */
#define CSI_NONE 0
#define CSI_ESC  1
#define CSI_BRK  2
static int g_esc_state = CSI_NONE;
static int g_csi_n = 0;
static int g_csi_has_n = 0;

static void clear_cols(uint32_t row, uint32_t x0, uint32_t x1)
{
    if (x1 > g_term_cols) x1 = g_term_cols;
    if (x0 >= x1) return;
    uint32_t px = CON_MARGIN + x0 * CHAR_W;
    uint32_t py = TITLE_H + CON_MARGIN + row * CHAR_H;
    fill_rect(px, py, (x1 - x0) * CHAR_W, CHAR_H, COL_WINBG);
}

static void clear_screen_area(void)
{
    fill_rect(CON_MARGIN + 2, TITLE_H + CON_MARGIN + 2,
              g_fb_width - CON_MARGIN * 2 - 4,
              g_fb_height - TITLE_H - CON_MARGIN * 2 - 4, COL_WINBG);
    g_term_x = 0; g_term_y = 0;
}

static void term_scroll_up(void)
{
    if (g_term_rows == 0) return;
    uint32_t left     = CON_MARGIN;
    uint32_t top      = TITLE_H + CON_MARGIN;
    uint32_t width_px = g_fb_width - CON_MARGIN * 2;
    uint32_t row_px   = CHAR_H;
    for (uint32_t y = 0; y + 1 < g_term_rows; y++) {
        uint32_t *dst = g_desk + (top + y * row_px) * g_stride + left;
        uint32_t *src = g_desk + (top + (y + 1) * row_px) * g_stride + left;
        memcpy(dst, src, (size_t)width_px * row_px * sizeof(uint32_t));
    }
    uint32_t *dst = g_desk + (top + (g_term_rows - 1) * row_px) * g_stride + left;
    for (uint32_t i = 0; i < width_px * row_px; i++) dst[i] = COL_WINBG;
}

static void csi_dispatch(char final)
{
    int n = g_csi_has_n ? g_csi_n : 1;
    switch (final) {
    case 'A': g_term_y = (g_term_y >= (uint32_t)n) ? g_term_y - (uint32_t)n : 0; break;
    case 'B':
        g_term_y += (uint32_t)n;
        if (g_term_y >= g_term_rows) g_term_y = g_term_rows ? g_term_rows - 1 : 0;
        break;
    case 'C':
        g_term_x += (uint32_t)n;
        if (g_term_x >= g_term_cols) g_term_x = g_term_cols ? g_term_cols - 1 : 0;
        break;
    case 'D': g_term_x = (g_term_x >= (uint32_t)n) ? g_term_x - (uint32_t)n : 0; break;
    case 'G':
        g_term_x = (g_csi_has_n && g_csi_n >= 1) ? (uint32_t)(g_csi_n - 1) : 0;
        if (g_term_x >= g_term_cols) g_term_x = g_term_cols ? g_term_cols - 1 : 0;
        break;
    case 'H': g_term_x = 0; g_term_y = 0; break;
    case 'J':
        if (g_csi_has_n && g_csi_n == 1 && g_term_y > 0) {
            for (uint32_t r = 0; r < g_term_y; r++) clear_cols(r, 0, g_term_cols);
            clear_cols(g_term_y, 0, g_term_x);
        } else clear_screen_area();
        break;
    case 'K':
        if (g_csi_has_n && g_csi_n == 2)      clear_cols(g_term_y, 0, g_term_cols);
        else if (g_csi_has_n && g_csi_n == 1) clear_cols(g_term_y, 0, g_term_x + 1);
        else                                  clear_cols(g_term_y, g_term_x, g_term_cols);
        break;
    default: break;
    }
}

static void term_putc(char c)
{
    if (g_esc_state == CSI_ESC) {
        if (c == '[') { g_esc_state = CSI_BRK; g_csi_n = 0; g_csi_has_n = 0; }
        else g_esc_state = CSI_NONE;
        return;
    }
    if (g_esc_state == CSI_BRK) {
        if (c >= '0' && c <= '9') {
            if (g_csi_n < 9999) g_csi_n = g_csi_n * 10 + (c - '0');
            g_csi_has_n = 1;
            return;
        }
        if (c == ';' || c == '?') return;
        g_esc_state = CSI_NONE;
        if (c >= '@' && c <= '~') csi_dispatch(c);
        return;
    }
    if (c == 0x1B) { g_esc_state = CSI_ESC; return; }

    if (c == '\r') { g_term_x = 0; return; }
    if (c == '\n') { g_term_x = 0; g_term_y++; }
    else if (c == '\t') {
        uint32_t ntab = 8 - (g_term_x % 8);
        for (uint32_t i = 0; i < ntab; i++) term_putc(' ');
        return;
    } else if (c == '\b') {
        if (g_term_x > 0) g_term_x--;
        else if (g_term_y > 0) { g_term_y--; g_term_x = g_term_cols - 1; }
        else return;
        uint32_t px = CON_MARGIN + g_term_x * CHAR_W;
        uint32_t py = TITLE_H + CON_MARGIN + g_term_y * CHAR_H;
        fill_rect(px, py, CHAR_W, CHAR_H, COL_WINBG);
        return;
    } else {
        uint32_t px = CON_MARGIN + g_term_x * CHAR_W;
        uint32_t py = TITLE_H + CON_MARGIN + g_term_y * CHAR_H;
        draw_glyph(px, py, (uint8_t)c, COL_FG);
        g_term_x++;
    }
    if (g_term_x >= g_term_cols) { g_term_x = 0; g_term_y++; }
    if (g_term_y >= g_term_rows) { term_scroll_up(); g_term_y = g_term_rows - 1; g_term_x = 0; }
}

static void term_puts(const char *s) { for (; *s; s++) term_putc(*s); }

static void draw_desktop(void)
{
    fill_rect(0, 0, g_fb_width, g_fb_height, COL_DESKTOP);
    fill_rect(0, 0, g_fb_width, TITLE_H, COL_BAR);
    const char *title = "SukiOS";
    uint32_t tx = CON_MARGIN;
    for (const char *p = title; *p; p++) { draw_glyph(tx, (TITLE_H - CHAR_H) / 2, (uint8_t)*p, COL_TITLE); tx += CHAR_W; }
    uint32_t wx = CON_MARGIN, wy = TITLE_H + CON_MARGIN;
    uint32_t ww = g_fb_width  - CON_MARGIN * 2;
    uint32_t wh = g_fb_height - TITLE_H - CON_MARGIN * 2;
    fill_rect(wx, wy, ww, wh, COL_WINBORDER);
    fill_rect(wx + 2, wy + 2, ww - 4, wh - 4, COL_WINBG);
    g_term_cols = (ww - 4) / CHAR_W;
    g_term_rows = (wh - 4) / CHAR_H;
    g_term_x = 0; g_term_y = 0;
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

static wm_window_t *wm_find(suki_window_id_t id)
{
    for (int i = 0; i < g_win_count; i++)
        if (g_wins[i].id == id) return &g_wins[i];
    return NULL;
}

/* 前向声明：合成器在窗口管理回调中被调用 */
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
    /* freestanding 下无 strncpy，手动拷贝窗口标题（长度受限） */
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
    /* 1) 桌面 + 终端层整体 blit */
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
        /* 窗口修饰：边框 + 标题栏（覆盖应用顶部 20px） */
        uint32_t border = 2;
        fill_rect_fb((uint32_t)x0, (uint32_t)y0, ww, border, COL_ACCENT);
        fill_rect_fb((uint32_t)x0, (uint32_t)y0 + hh - border, ww, border, COL_ACCENT);
        fill_rect_fb((uint32_t)x0, (uint32_t)y0, border, hh, COL_ACCENT);
        fill_rect_fb((uint32_t)x0 + ww - border, (uint32_t)y0, border, hh, COL_ACCENT);
        if (w->style & SUKI_WS_TITLEBAR) {
            uint32_t bar_h = 20;
            if (hh >= bar_h) {
                fill_rect_fb((uint32_t)x0 + border, (uint32_t)y0 + border, ww - border * 2, bar_h - border, COL_BAR);
                uint32_t tx = (uint32_t)x0 + border + 4, ty = (uint32_t)y0 + border + 6;
                for (uint32_t k = 0; k < sizeof(w->title) && w->title[k]; k++)
                    draw_glyph_fb(tx + k * 8, ty, (uint8_t)w->title[k],
                                 w->has_focus ? 0x00FFFFFF : 0x009090A0);
            }
        }
    }
    /* 3) 光标（最上层） */
    draw_cursor_on_fb();
}

/* 启动日志（类似 dmesg） */
static void drain_console_pipe(void)
{
    static char buf[512];
    for (;;) {
        uint64_t n = suki_syscall2(SYS_CONSOLE_READ, (uint64_t)buf, sizeof(buf) - 1);
        if (n == (uint64_t)-1 || n == 0) break;
        buf[n] = '\0';
        term_puts(buf);
        if (n < sizeof(buf) - 1) break;
    }
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

    /* 离屏桌面缓冲（与帧缓冲同尺寸） */
    g_desk = (uint32_t *)sys_mmap((uint64_t)g_fb_height * g_stride * sizeof(uint32_t), 3);
    if (!g_desk) { u_print("display: desk alloc failed\n"); sys_exit(1); }

    u_print("display: fb mapped @32bpp, wm ready\n");

    /* 3) 绘制桌面（到 g_desk），写启动提示 */
    draw_desktop();
    term_puts("SukiOS display + window manager ready.\n");
    term_puts("Kernel boot log:\n");

    /* 4) 通知内核：显示服务已接管帧缓冲 */
    suki_syscall1(SYS_DISPLAY_READY, 0);

    /* 5) 刷内核启动日志 */
    drain_console_pipe();

    /* 6) 消息循环：同时轮询 DISPLAY_PORT（文本/鼠标）与 WM_PORT（窗口管理） */
    static uint8_t msgbuf[512];
    typedef struct { mach_msg_header_t h; int32_t x; int32_t y; uint8_t buttons; int8_t wheel; uint8_t _pad[3]; } mouse_event_msg_t;

    for (;;) {
        /* 显示端口：文本与鼠标事件 */
        if (mach_msg_tryrecv(msgbuf, sizeof(msgbuf), DISPLAY_PORT) == 0) {
            mach_msg_header_t *h = (mach_msg_header_t *)msgbuf;
            if (h->msgh_id == DISP_MSG_TEXT) {
                term_puts((char *)msgbuf + sizeof(mach_msg_header_t));
                composite();
            } else if (h->msgh_id == MOUSE_MSG_MOVE ||
                       h->msgh_id == MOUSE_MSG_BUTTON ||
                       h->msgh_id == MOUSE_MSG_WHEEL) {
                mouse_event_msg_t *m = (mouse_event_msg_t *)msgbuf;
                g_cur_x = m->x; g_cur_y = m->y;
                if (h->msgh_id == MOUSE_MSG_BUTTON) g_cur_buttons = m->buttons;
                /* 命中测试 + 焦点 + 事件转发 */
                wm_window_t *hit = wm_hit_test(m->x, m->y);
                for (int i = 0; i < g_win_count; i++) g_wins[i].has_focus = false;
                if (hit) {
                    hit->has_focus = true;
                    suki_event_t ev; memset(&ev, 0, sizeof(ev));
                    if (h->msgh_id == MOUSE_MSG_MOVE) {
                        ev.type = SUKI_EVENT_MOUSE_MOVE;
                        ev.u.mouse.x = m->x - hit->x; ev.u.mouse.y = m->y - hit->y;
                        ev.u.mouse.buttons = g_cur_buttons;
                    } else {
                        ev.type = (g_cur_buttons & 1) ? SUKI_EVENT_MOUSE_DOWN : SUKI_EVENT_MOUSE_UP;
                        ev.u.mouse.x = m->x - hit->x; ev.u.mouse.y = m->y - hit->y;
                        ev.u.mouse.buttons = g_cur_buttons;
                    }
                    wm_forward_event(hit, &ev);
                }
                composite();
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
            default:
                break;
            }
        }

        sys_yield();
    }
    return 0;
}
