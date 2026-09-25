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

/* 增量渲染：维护脏矩形列表，仅把变化区域重新合成并拷贝到帧缓冲 */
static void mark_dirty(int x, int y, int w, int h);
static void flush_dirty(void);

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
static uint32_t *g_canvas   = NULL;      /* 合成画布（双缓冲后缓冲，指向 g_desk） */

/* 颜色（xRGB32；背景纯黑，已删除桌面装饰条/菜单栏/任务栏，仅做窗口管理+合成） */
#define COL_DESKTOP       0x00000000   /* 帧缓冲背景 = 纯黑 */
#define COL_BAR_INACTIVE  0x003A2A4A   /* 非活动窗口标题栏（iSuki 中性紫灰） */
#define COL_ACCENT        0x00579BFE   /* iSuki 活动态强调蓝（accent） */
#define COL_TL_CLOSE      0x00FF5F57   /* 交通灯：关闭（红） */
#define COL_TL_MIN        0x00FEBC2E   /* 交通灯：最小化（黄） */
#define COL_TL_MAX        0x0028C840   /* 交通灯：最大化（绿） */

/* 背景纯黑。显示服务不渲染任何桌面元素（装饰条 / 菜单栏 / 任务栏等），
 * 仅做窗口管理与合成，故此处只填纯黑；未变化的区域完全不重绘。 */
static void draw_desktop(void)
{
    for (uint32_t y = 0; y < g_fb_height; y++) {
        uint32_t *row = g_desk + (uint64_t)y * g_stride;
        for (uint32_t x = 0; x < g_fb_width; x++) row[x] = COL_DESKTOP;
    }
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

/* 窗口装饰尺寸（与 iSuki 规范 §12.1 对齐：标题栏高 38、圆角 12） */
#define WIN_TITLE_H 38
#define WIN_RADIUS  12

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
    if (need == 0 || need > 2048 * 4096) goto fail;   /* OOL ≤2048 页(8MiB) */
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
    mark_dirty(w->x, w->y, (int)w->w, (int)w->h);
    flush_dirty();
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
    if (!w) { u_print("[display] FLUSH: unknown id\n"); return; }
    uint32_t *src = (uint32_t *)(uintptr_t)f->ool.address;
    uint64_t cp = (uint64_t)w->w * w->h * 4;
    if (f->ool.size < cp) cp = f->ool.size;
    memcpy(w->pixels, src, cp);
    /* 解映射收到的 OOL 物理页（引用计数 -1）；mach_msg_destroy 仅取 OOL 虚拟地址 */
    mach_msg_destroy((uint64_t)f->ool.address);
    mark_dirty((int)w->x, (int)w->y, (int)w->w, (int)w->h);
    flush_dirty();
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
    /* 销毁前先记录旧位置（数组移除后会覆盖该槽，故提前保存供增量重绘） */
    int ddx = w->x, ddy = w->y; uint32_t ddw = w->w, ddh = w->h;
    uint64_t need = (uint64_t)w->w * w->h * 4;
    sys_munmap(w->pixels, need);
    /* 从数组中移除（保留顺序=Z 稳定） */
    int idx = (int)(w - g_wins);
    for (int i = idx; i + 1 < g_win_count; i++) g_wins[i] = g_wins[i + 1];
    g_win_count--;
    u_print("[wm] window destroyed\n");
    mark_dirty(ddx, ddy, (int)ddw, (int)ddh);
    flush_dirty();
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

/* 交通灯命中：标题栏左侧三色圆点。返回 1=关闭(红) 2=最小化(黄) 3=最大化(绿) 0=无 */
static int wm_traffic_light_at(const wm_window_t *w, int32_t sx, int32_t sy)
{
    if (!(w->style & SUKI_WS_TITLEBAR)) return 0;
    int32_t cy = w->y + (int32_t)(WIN_TITLE_H / 2);   /* 标题栏竖直中心 */
    int32_t xs[3] = { w->x + 14, w->x + 34, w->x + 54 };
    for (int i = 0; i < 3; i++) {
        int32_t dx = sx - xs[i], dy = sy - cy;
        if (dx*dx + dy*dy <= 7*7) return i + 1;        /* 命中半径 7，便于点击 */
    }
    return 0;
}

/* 设置焦点窗口并向相关窗口广播 SUKI_EVENT_WINDOW_FOCUS（buttons:1=得焦点,0=失焦点） */
static void wm_set_focus(wm_window_t *w)
{
    if (g_focus == w) return;
    if (g_focus) {
        g_focus->has_focus = false;
        mark_dirty(g_focus->x, g_focus->y, (int)g_focus->w, (int)g_focus->h);
        suki_event_t ev; memset(&ev, 0, sizeof(ev));
        ev.type = SUKI_EVENT_WINDOW_FOCUS;
        ev.u.mouse.buttons = 0;
        wm_forward_event(g_focus, &ev);
    }
    g_focus = w;
    if (g_focus) {
        g_focus->has_focus = true;
        mark_dirty(g_focus->x, g_focus->y, (int)g_focus->w, (int)g_focus->h);
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

/* 圆角由应用（libsui）自行绘制：窗口像素缓冲四角已填窗口背景色。display_server
 * 不再挖洞/填黑四角，直接整块拷贝应用像素，避免把应用圆角覆盖成黑色（黑边朝内）。 */

/* 直写真实帧缓冲的单像素（光标只画到显存，绝不被写入离屏层） */
static inline void fb_put_px(int32_t x, int32_t y, uint32_t rgb)
{
    if (x < 0 || y < 0 || (uint32_t)x >= g_fb_width || (uint32_t)y >= g_fb_height) return;
    ((uint32_t *)g_fb)[(uint64_t)y * g_stride + (uint32_t)x] = rgb;
}

/* 在指定位置把光标画到真实帧缓冲 */
static void draw_cursor_at(int32_t ox, int32_t oy)
{
    uint32_t col = (g_cur_buttons & 1) ? 0x00FF3030 : 0x00FFFFFF;
    for (int32_t j = 0; j < MOUSE_CURSOR_H; j++)
        for (int32_t i = 0; i < MOUSE_CURSOR_W; i++)
            if (g_cursor_mask[j][i]) fb_put_px(ox + i, oy + j, col);
}

/* 鼠标移动不再使用 fb_restore_row_from_desk / cursor_move_only 直接覆盖显存；
 * 改为在消息循环中把旧/新光标矩形 mark_dirty，由增量合成器重绘（见下方
 * MOUSE_MSG_MOVE 处理），保证旧光标位置被正确重绘为下层窗口内容，并与圆角
 * 挖洞状态保持一致。
 */

/* ===========================================================================
 * 增量合成器
 * - 维护脏矩形列表；仅把变化区域重新合成并拷贝到帧缓冲（未变化的区域与对应
 *   显存完全不重绘、不拷贝）。
 * - 背景 = 纯黑（draw_desktop 只填黑），故清脏区域即填黑。
 * - 光标单独画到显存（不参与离屏层，鼠标移动可据此做局部擦除）。
 * ======================================================================== */
#define MAX_DIRTY 8
static int  g_dirty_n = 0;
static int  g_dirty[MAX_DIRTY][4];   /* x, y, w, h */
/* 当前合成裁剪窗（窗口局部刷新 / 移动时只绘制相交区域） */
static int  g_clx, g_cly, g_clw, g_clh;

/* 在 g_canvas（=g_desk）上把矩形 [x,y,w,h] 与裁剪窗 + 屏幕相交后填充 rgb */
static void fill_clip(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb)
{
    int cx0 = g_clx, cy0 = g_cly, cx1 = g_clx + g_clw, cy1 = g_cly + g_clh;
    int x1 = (int)(x + w), y1 = (int)(y + h);
    if ((int)x >= cx1 || (int)y >= cy1 || x1 <= cx0 || y1 <= cy0) return;
    int bx = (int)x < cx0 ? cx0 : (int)x;
    int by = (int)y < cy0 ? cy0 : (int)y;
    int ex = x1 > cx1 ? cx1 : x1;
    int ey = y1 > cy1 ? cy1 : y1;
    if (bx < 0) bx = 0;
    if (by < 0) by = 0;
    if (ex > (int)g_fb_width)  ex = (int)g_fb_width;
    if (ey > (int)g_fb_height) ey = (int)g_fb_height;
    for (int j = by; j < ey; j++) {
        uint32_t *row = g_canvas + (uint64_t)j * g_stride;
        for (int i = bx; i < ex; i++) row[i] = rgb;
    }
}

/* 在裁剪窗内填充一个实心圆（交通灯用），仅触碰裁剪窗内的像素 */
static void wm_fill_disc(int32_t cx, int32_t cy, int32_t rad, uint32_t color)
{
    int cx0 = g_clx, cy0 = g_cly, cx1 = g_clx + g_clw, cy1 = g_cly + g_clh;
    int x0 = cx - rad, y0 = cy - rad, x1 = cx + rad, y1 = cy + rad;
    if (x1 <= cx0 || y1 <= cy0 || x0 >= cx1 || y0 >= cy1) return;
    int bx = x0 < cx0 ? cx0 : x0;
    int by = y0 < cy0 ? cy0 : y0;
    int ex = x1 > cx1 ? cx1 : x1;
    int ey = y1 > cy1 ? cy1 : y1;
    if (bx < 0) bx = 0;
    if (by < 0) by = 0;
    if (ex > (int)g_fb_width)  ex = (int)g_fb_width;
    if (ey > (int)g_fb_height) ey = (int)g_fb_height;
    int r2 = rad * rad;
    for (int j = by; j < ey; j++)
        for (int i = bx; i < ex; i++) {
            int32_t dx = i - cx, dy = j - cy;
            if (dx*dx + dy*dy <= r2) {
                if (i >= 0 && j >= 0 && i < (int32_t)g_fb_width && j < (int32_t)g_fb_height)
                    g_canvas[(uint64_t)j * g_stride + i] = color;
            }
        }
}

/* 圆角由应用（libsui）在窗口像素缓冲中绘制，display_server 整块拷贝，不再做圆角判定。 */

/* 绘制单个窗口的像素 + 几何装饰，仅输出与裁剪窗相交部分（背景黑由调用方清除） */
static void wm_paint_window(wm_window_t *w)
{
    if (!w->visible) return;
    int32_t x0 = w->x, y0 = w->y;
    uint32_t ww = w->w, hh = w->h;
    int c0x = g_clx, c0y = g_cly, c1x = g_clx + g_clw, c1y = g_cly + g_clh;
    int wx1 = (int)(x0 + ww), wy1 = (int)(y0 + hh);
    if (x0 >= c1x || y0 >= c1y || wx1 <= c0x || wy1 <= c0y) return;  /* 不相交 */
    int by0 = y0 < c0y ? c0y : y0;
    int by1 = wy1 > c1y ? c1y : wy1;
    int bx0 = x0 < c0x ? c0x : x0;
    int bx1 = wx1 > c1x ? c1x : wx1;
    if (by0 < 0) by0 = 0;
    if (bx0 < 0) bx0 = 0;
    if (by1 > (int)g_fb_height) by1 = (int)g_fb_height;
    if (bx1 > (int)g_fb_width)  bx1 = (int)g_fb_width;
    /* 像素拷贝：直接整块拷贝应用像素（含应用自绘圆角四角背景），不做圆角挖洞，
     * 避免覆盖应用已画好的圆角（否则四角被填黑/透黑，表现为黑边朝内）。 */
    for (int y = by0; y < by1; y++) {
        const uint32_t *s = w->pixels + (uint64_t)(y - y0) * ww + (uint32_t)(bx0 - x0);
        uint32_t *d = g_canvas + (uint64_t)y * g_stride + (uint32_t)bx0;
        memcpy(d, s, (size_t)(bx1 - bx0) * 4);
    }
    /* 窗口装饰（iSuki 新样式，纯几何无文字）：1px 边框 + 38px 标题栏 + 左侧三色交通灯 + 圆角(12) */
    uint32_t b = 1;
    fill_clip((uint32_t)x0, (uint32_t)y0, ww, b, COL_ACCENT);
    fill_clip((uint32_t)x0, (uint32_t)y0 + hh - b, ww, b, COL_ACCENT);
    fill_clip((uint32_t)x0, (uint32_t)y0, b, hh, COL_ACCENT);
    fill_clip((uint32_t)x0 + ww - b, (uint32_t)y0, b, hh, COL_ACCENT);
    if (w->style & SUKI_WS_TITLEBAR) {
        uint32_t bar_h = WIN_TITLE_H;
        if (hh >= bar_h) {
            /* 标题栏底（边框内侧，避免压住 1px 边框） */
            fill_clip((uint32_t)x0 + b, (uint32_t)y0 + b, ww - b * 2, bar_h - b,
                      w->has_focus ? COL_ACCENT : COL_BAR_INACTIVE);
            /* 左侧三色交通灯（macOS 风格：红/黄/绿，间距 8，直径 12） */
            int32_t cy = (int32_t)y0 + (int32_t)(WIN_TITLE_H / 2);
            wm_fill_disc((int32_t)x0 + 14, cy, 6, COL_TL_CLOSE);
            wm_fill_disc((int32_t)x0 + 34, cy, 6, COL_TL_MIN);
            wm_fill_disc((int32_t)x0 + 54, cy, 6, COL_TL_MAX);
        }
    }
    /* 圆角由应用（libsui）在像素缓冲中绘制；本服务仅整块拷贝，不做圆角挖洞/填黑。 */
}

/* 重绘单个脏矩形：清黑 -> 重绘相交窗口 -> 仅拷贝该区域到帧缓冲 */
static void redraw_region(int rx, int ry, int rw, int rh)
{
    if (!g_fb || !g_desk) return;
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rw <= 0 || rh <= 0) return;
    if (rx + rw > (int)g_fb_width)  rw = (int)g_fb_width - rx;
    if (ry + rh > (int)g_fb_height) rh = (int)g_fb_height - ry;
    if (rw <= 0 || rh <= 0) return;
    g_canvas = g_desk;
    g_clx = rx; g_cly = ry; g_clw = rw; g_clh = rh;
    /* 清黑（仅该区域） */
    for (int y = ry; y < ry + rh; y++)
        memset(g_canvas + (uint64_t)y * g_stride + rx, 0, (size_t)rw * 4);
    /* 重绘相交窗口（Z-order = 创建顺序，后者在上） */
    for (int i = 0; i < g_win_count; i++) wm_paint_window(&g_wins[i]);
    /* 仅拷贝该区域到帧缓冲（离屏层 g_desk 始终【不含】光标） */
    for (int y = ry; y < ry + rh; y++) {
        const uint32_t *s = g_desk + (uint64_t)y * g_stride + rx;
        uint32_t *d = (uint32_t *)g_fb + (uint64_t)y * g_stride + rx;
        memcpy(d, s, (size_t)rw * 4);
    }
}

void mark_dirty(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    if (g_dirty_n >= MAX_DIRTY) {   /* 超出上限：退化为全屏脏矩形 */
        g_dirty[0][0] = 0; g_dirty[0][1] = 0;
        g_dirty[0][2] = (int)g_fb_width; g_dirty[0][3] = (int)g_fb_height;
        g_dirty_n = 1; return;
    }
    g_dirty[g_dirty_n][0] = x; g_dirty[g_dirty_n][1] = y;
    g_dirty[g_dirty_n][2] = w; g_dirty[g_dirty_n][3] = h;
    g_dirty_n++;
}

/* 处理所有累计脏矩形（增量提交到帧缓冲），最后重画光标 */
static void flush_dirty(void)
{
    if (g_dirty_n == 0) return;
    for (int i = 0; i < g_dirty_n; i++)
        redraw_region(g_dirty[i][0], g_dirty[i][1],
                      g_dirty[i][2], g_dirty[i][3]);
    draw_cursor_at(g_cur_x, g_cur_y);   /* 光标最上层，只画到显存 */
    g_dirty_n = 0;
}

/* 全屏合成（仅启动交接 / 显式全量刷新用） */
static void composite(void)
{
    if (!g_fb || !g_desk) return;
    g_canvas = g_desk;
    draw_desktop();
    g_clx = 0; g_cly = 0; g_clw = (int)g_fb_width; g_clh = (int)g_fb_height;
    for (int i = 0; i < g_win_count; i++) wm_paint_window(&g_wins[i]);
    memcpy((void *)g_fb, g_desk, (size_t)g_fb_height * g_stride * sizeof(uint32_t));
    draw_cursor_at(g_cur_x, g_cur_y);
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

    /* 3) 画布指向背景缓冲，绘制背景层（双缓冲：合成到 g_desk 后一次性提交）。
     *    注意：这里只画【离屏】缓冲，不碰显存——此刻内核正显示开机动画。 */
    g_canvas = g_desk;
    draw_desktop();

    /* 4) 通知内核：显示服务已接管帧缓冲（此后内核诊断改走环形管道，不再写屏，
     *    开机动画不会被诊断文本涂抹） */
    suki_syscall1(SYS_DISPLAY_READY, 0);
    u_print("display: fb mapped @32bpp, pure compositor ready\n");

    /* 4a) 开机动画交接闸门：在内核完成【驱动 + 全部 Ring3 服务初始化 + 启动画面
     *     收尾】之前，屏幕仍由内核的开机动画（启动图 + 进度条）占用；本服务在此
     *     阻塞等待（内核用 msleep 让出 CPU，不忙等）。返回即表示系统启动完成，
     *     可以提交桌面首帧并进入消息循环，随后进入用户登录/桌面流程。 */
    suki_syscall1(SYS_BOOT_SPLASH_WAIT, 0);

    /* 4b) 干净接手屏幕：提交首帧桌面（此前显存仍是开机动画画面） */
    composite();

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
                int32_t oldx = g_cur_x, oldy = g_cur_y;   /* 供 MOVE 脏矩形擦除 */
                g_cur_x = m->x; g_cur_y = m->y;
                if (h->msgh_id == MOUSE_MSG_BUTTON) g_cur_buttons = m->buttons;

                wm_window_t *hit = wm_hit_test(m->x, m->y);

                /* 拖拽进行中：直接跟随光标移动窗口（忽略命中） */
                if (g_drag) {
                    if (h->msgh_id == MOUSE_MSG_MOVE) {
                        int32_t ox = g_drag->x, oy = g_drag->y;
                        g_drag->x = m->x - g_drag_offx;
                        g_drag->y = m->y - g_drag_offy;
                        mark_dirty(ox, oy, (int)g_drag->w, (int)g_drag->h);
                        mark_dirty(g_drag->x, g_drag->y, (int)g_drag->w, (int)g_drag->h);
                        mark_dirty(oldx, oldy, MOUSE_CURSOR_W, MOUSE_CURSOR_H);  /* 旧光标位置 */
                        flush_dirty();
                    } else if (h->msgh_id == MOUSE_MSG_BUTTON) {
                        /* 左键释放 -> 结束拖拽；并视情况下发 MOUSE_UP 给窗口 */
                        if (!(m->buttons & 1)) {
                            g_drag = NULL;
                            if (hit) {
                                suki_event_t ev; memset(&ev, 0, sizeof(ev));
                                ev.type = SUKI_EVENT_MOUSE_UP;
                                ev.u.mouse.x = m->x - hit->x;
                                ev.u.mouse.y = m->y - hit->y;
                                ev.u.mouse.buttons = m->buttons;
                                wm_forward_event(hit, &ev);
                            }
                            flush_dirty();
                        }
                    }
                    continue;
                }

                if (h->msgh_id == MOUSE_MSG_BUTTON && (m->buttons & 1)) {
                    /* 左键按下：标题栏 -> 关闭按钮 / 拖拽；客户区 -> 聚焦 + 下发 */
                    if (hit && (hit->style & SUKI_WS_TITLEBAR)) {
                        int tl = wm_traffic_light_at(hit, m->x, m->y);
                        if (tl == 1) {   /* 红灯：关闭窗口 */
                            suki_event_t ev; memset(&ev, 0, sizeof(ev));
                            ev.type = SUKI_EVENT_WINDOW_CLOSE;
                            wm_forward_event(hit, &ev);
                            flush_dirty();
                            continue;
                        }
                        if (tl == 2 || tl == 3) {  /* 黄/绿灯：最小化/最大化协议未实现，仅消费点击 */
                            flush_dirty();
                            continue;
                        }
                        if (m->y >= hit->y && m->y < hit->y + (int32_t)WIN_TITLE_H) {
                            g_drag = hit;
                            g_drag_offx = m->x - hit->x;
                            g_drag_offy = m->y - hit->y;
                            wm_set_focus(hit);
                            flush_dirty();
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
                    flush_dirty();
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
                    flush_dirty();
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
                    if (h->msgh_id == MOUSE_MSG_MOVE) {
                        /* 鼠标移动：旧光标矩形与新光标矩形都标记为脏，交给增量合成器
                         * 重绘（重绘会恢复下层窗口内容并清除圆角残影），末尾统一画新光标。
                         * 不再用 fb_restore_row_from_desk 直接覆盖，避免与增量状态不一致。 */
                        mark_dirty(oldx, oldy, MOUSE_CURSOR_W, MOUSE_CURSOR_H);
                        mark_dirty(g_cur_x, g_cur_y, MOUSE_CURSOR_W, MOUSE_CURSOR_H);
                        flush_dirty();
                    } else {
                        flush_dirty();                  /* WHEEL 不改像素，仅清脏 */
                    }
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
                if (w) {
                    mark_dirty(w->x, w->y, (int)w->w, (int)w->h);
                    w->x = r->x; w->y = r->y;
                    mark_dirty(w->x, w->y, (int)w->w, (int)w->h);
                    flush_dirty();
                }
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
