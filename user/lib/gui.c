/*
 * user/lib/gui.c
 * -----------------------------------------------------------------------------
 * libsuki_gui 客户端实现（SukiOS 用户态 GUI 库）。
 *
 * 双形态构建：
 *   - 作为 .sl 共享库（ld -shared）供磁盘应用动态链接；
 *   - 作为普通 .o 静态链入内嵌自检程序（winhello）以便开机自动验证。
 *
 * 所有跨特权/跨进程交互均走合法 syscall + Mach IPC，绝不直接访问硬件。
 * 窗口离屏缓冲由本库用 sys_mmap 分配（页对齐、≤16 页），提交经 OOL 零拷贝。
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "lib/suki.h"
#include "lib/suki_gui.h"
#include "lib/font8x8.h"

#define GUI_PAGE 4096
#define GUI_MAX_WINDOWS 16

/* 窗口句柄池（freestanding 无 malloc，静态池即可满足基础场景） */
static suki_window_t g_gui_wins[GUI_MAX_WINDOWS];
static int g_gui_wins_used = 0;

/* 极简字符串拷贝（不依赖 libc 符号） */
static void my_strncpy(char *d, const char *s, size_t n)
{
    size_t i = 0;
    for (; i + 1 < n && s[i]; i++) d[i] = s[i];
    if (n) d[i] = '\0';
}

/* 同步 IPC 调用：临时分配应答端口，发送后等待应答 */
static int gui_ipc_call(uint32_t remote, uint32_t id,
                        void *req, uint32_t reqsz,
                        void *resp, uint32_t respsz)
{
    uint32_t rp = sys_port_alloc();
    if (rp == 0) return -1;
    sys_port_claim(rp);
    mach_msg_header_t *h = (mach_msg_header_t *)req;
    h->msgh_bits       = MACH_SEND_MSG;
    h->msgh_size       = reqsz;
    h->msgh_remote_port = remote;
    h->msgh_local_port  = rp;
    h->msgh_id          = id;
    mach_msg_send(req, reqsz);
    uint64_t r = mach_msg_recv(resp, respsz, rp);
    sys_port_free(rp);
    return (r == 0) ? 0 : -1;
}

suki_window_t *suki_create_window(const char *title, int x, int y,
                                  uint32_t w, uint32_t h, uint32_t style)
{
    uint64_t need = (uint64_t)w * h * 4;
    if (need == 0 || need > 16 * GUI_PAGE) return NULL;   /* OOL ≤16 页上限 */

    if (g_gui_wins_used >= GUI_MAX_WINDOWS) return NULL;
    suki_window_t *win = &g_gui_wins[g_gui_wins_used++];

    void *buf = sys_mmap(need, 3);   /* PROT_READ|PROT_WRITE */
    if (!buf) { g_gui_wins_used--; return NULL; }

    win->id = SUKI_WINDOW_ID_INVALID;
    win->x = x; win->y = y; win->w = w; win->h = h; win->style = style;
    win->buffer = buf; win->buffer_size = (uint32_t)need; win->event_port = 0;
    my_strncpy(win->title, title ? title : "", sizeof(win->title) - 1);

    wm_create_req_t req; memset(&req, 0, sizeof(req));
    req.x = x; req.y = y; req.w = w; req.height = h; req.style = style; req.event_port = 0;
    my_strncpy(req.title, win->title, sizeof(req.title) - 1);

    wm_create_resp_t resp; memset(&resp, 0, sizeof(resp));
    if (gui_ipc_call(WM_PORT, WM_MSG_CREATE, &req, sizeof(req),
                     &resp, sizeof(resp)) != 0 || resp.status != 0) {
        sys_munmap(buf, need);
        g_gui_wins_used--;
        return NULL;
    }
    win->id = resp.id;
    return win;
}

void suki_destroy_window(suki_window_t *w)
{
    if (!w || w->id == SUKI_WINDOW_ID_INVALID) return;
    wm_destroy_req_t m; memset(&m, 0, sizeof(m));
    m.h.msgh_bits        = MACH_SEND_MSG;
    m.h.msgh_size        = sizeof(m);
    m.h.msgh_remote_port = WM_PORT;
    m.h.msgh_local_port  = 0;
    m.h.msgh_id          = WM_MSG_DESTROY;
    m.id = w->id;
    mach_msg_send(&m, sizeof(m));
    sys_munmap(w->buffer, w->buffer_size);
    w->id = SUKI_WINDOW_ID_INVALID;
    w->buffer = NULL;
}

void *suki_get_buffer(suki_window_t *w) { return w ? w->buffer : NULL; }

void suki_set_pixel(suki_window_t *w, int x, int y, uint32_t rgb)
{
    if (!w || x < 0 || y < 0 || (uint32_t)x >= w->w || (uint32_t)y >= w->h) return;
    ((uint32_t *)w->buffer)[y * w->w + x] = rgb;
}

void suki_fill_rect(suki_window_t *w, int x, int y, uint32_t ww, uint32_t hh, uint32_t rgb)
{
    if (!w) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (int)(x + ww) > (int)w->w ? (int)w->w : (int)(x + ww);
    int y1 = (int)(y + hh) > (int)w->h ? (int)w->h : (int)(y + hh);
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++)
            ((uint32_t *)w->buffer)[j * w->w + i] = rgb;
}

void suki_draw_line(suki_window_t *w, int x0, int y0, int x1, int y1, uint32_t rgb)
{
    if (!w) return;
    int dx = x1 - x0, dy = y1 - y0;
    int steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx)
                                                            : (dy < 0 ? -dy : dy);
    if (steps == 0) { suki_set_pixel(w, x0, y0, rgb); return; }
    for (int s = 0; s <= steps; s++) {
        int x = x0 + (int)((int64_t)dx * s / steps);
        int y = y0 + (int)((int64_t)dy * s / steps);
        suki_set_pixel(w, x, y, rgb);
    }
}

void suki_draw_text(suki_window_t *w, int x, int y, const char *text, uint32_t rgb)
{
    if (!w || !text) return;
    int cx = x;
    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c > 0x7E) c = '?';
        const uint8_t *g = font8x8_basic[c];
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++)
                if (g[row] & (1u << col))
                    suki_set_pixel(w, cx + col, y + row, rgb);
        cx += 8;
    }
}

void suki_flush(suki_window_t *w, int x, int y, uint32_t ww, uint32_t hh)
{
    if (!w || w->id == SUKI_WINDOW_ID_INVALID) return;
    wm_flush_req_t m; memset(&m, 0, sizeof(m));
    m.h.msgh_bits        = MACH_SEND_MSG | MACH_MSGH_BITS_OOL;
    m.h.msgh_size        = sizeof(m);
    m.h.msgh_remote_port = WM_PORT;
    m.h.msgh_local_port  = 0;
    m.h.msgh_id          = WM_MSG_FLUSH;
    m.ool.address = (uint64_t)w->buffer;
    m.ool.size   = w->buffer_size;
    m.id = w->id; m.x = (uint32_t)x; m.y = (uint32_t)y; m.w = ww; m.height = hh;
    mach_msg_send(&m, sizeof(m));
}

void suki_set_event_port(suki_window_t *w, uint32_t port)
{
    if (!w || w->id == SUKI_WINDOW_ID_INVALID) return;
    wm_set_event_req_t m; memset(&m, 0, sizeof(m));
    m.h.msgh_bits        = MACH_SEND_MSG;
    m.h.msgh_size        = sizeof(m);
    m.h.msgh_remote_port = WM_PORT;
    m.h.msgh_local_port  = 0;
    m.h.msgh_id          = WM_MSG_SET_EVENT;
    m.id = w->id; m.event_port = port;
    mach_msg_send(&m, sizeof(m));
    w->event_port = port;
}

bool suki_poll_event(suki_window_t *w, suki_event_t *out)
{
    if (!w || w->event_port == 0 || !out) return false;
    static uint8_t buf[256];
    uint64_t r = mach_msg_tryrecv(buf, sizeof(buf), w->event_port);
    if (r != 0) return false;
    mach_msg_header_t *h = (mach_msg_header_t *)buf;
    if (h->msgh_id != WM_MSG_EVENT) return false;
    wm_event_msg_t *e = (wm_event_msg_t *)buf;
    *out = e->event;
    return true;
}

int suki_set_focus(suki_window_t *w)
{
    if (!w || w->id == SUKI_WINDOW_ID_INVALID) return -1;
    wm_set_focus_req_t req; memset(&req, 0, sizeof(req));
    wm_set_focus_resp_t resp; memset(&resp, 0, sizeof(resp));
    if (gui_ipc_call(WM_PORT, WM_MSG_SET_FOCUS, &req, sizeof(req),
                     &resp, sizeof(resp)) != 0 || resp.status != 0)
        return -1;
    return 0;
}

void suki_set_window_pos(suki_window_t *w, int x, int y)
{
    if (!w || w->id == SUKI_WINDOW_ID_INVALID) return;
    wm_set_pos_req_t m; memset(&m, 0, sizeof(m));
    m.h.msgh_bits        = MACH_SEND_MSG;
    m.h.msgh_size        = sizeof(m);
    m.h.msgh_remote_port = WM_PORT;
    m.h.msgh_local_port  = 0;
    m.h.msgh_id          = WM_MSG_SET_POS;
    m.id = w->id; m.x = (int32_t)x; m.y = (int32_t)y;
    mach_msg_send(&m, sizeof(m));
    w->x = (int32_t)x; w->y = (int32_t)y;
}

int suki_get_window_rect(suki_window_t *w, int32_t *x, int32_t *y,
                         uint32_t *w_out, uint32_t *h_out)
{
    if (!w) return -1;
    if (x)    *x    = w->x;
    if (y)    *y    = w->y;
    if (w_out)*w_out = w->w;
    if (h_out)*h_out = w->h;
    return 0;
}
