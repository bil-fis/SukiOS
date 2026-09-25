/*
 * user/libsui/src/sui_font.c
 * -----------------------------------------------------------------------------
 * libsui 字体客户端：把文本渲染委托给常驻字体服务 fontsrv（见 font_ipc.h）。
 *
 * 设计（v2）：
 *   - libsui 不再自带 FreeType 合成后端（iSukiUI「不要自带合成器」要求）；所有字形
 *     光栅化 / alpha 合成由 fontsrv 在其自身地址空间内完成。
 *   - 首次绘制时，libsui 把当前窗口离屏画布（c->pixels，即 gui.c 用 sys_mmap 分配
 *     的缓冲）以 OOL 物理页注册给 fontsrv（持久映射），拿到 handle；
 *   - 之后每次 sui_font_draw 仅发一条小消息（handle + 坐标/字号/颜色/裁剪/文本），
 *     fontsrv 把字形合成进该画布；sui_font_measure 经 FONT_MSG_MEASURE 取宽度。
 *   - 若 fontsrv 暂未就绪（如启动早期），发送会重试（sys_yield）直至其上线；若始终
 *     不可用，返回失败，由调用方（sui_canvas_draw_text）降级为 ASCII 点阵。
 *
 * 依赖：libsui 链接时不依赖 FreeType；fontsrv 单独链接 libfreetype.a。
 */
#include "sui.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "lib/suki.h"
#include "font_ipc.h"

static char    g_font_path[256];
static bool    g_fontsrv_ok = false;
static uint32_t g_reply_port = 0;

/* 缓冲注册表：client 画布 VA -> fontsrv 返回的 handle */
#define SUI_FREG_MAX 8
typedef struct { void *va; uint32_t handle; int w, h; bool used; } sui_freg_t;
static sui_freg_t g_regs[SUI_FREG_MAX];

/* 受限字符串拷贝（避免引入 libc strncpy 符号依赖） */
static void sui_font_cpy(char *d, const char *s, int n)
{
    int i = 0;
    while (s && s[i] && i < n - 1) { d[i] = s[i]; i++; }
    if (i < n) d[i] = 0;
}

static uint32_t get_reply_port(void)
{
    if (!g_reply_port) {
        g_reply_port = (uint32_t)sys_port_alloc();
        if (g_reply_port) sys_port_claim(g_reply_port);
    }
    return g_reply_port;
}

/* 发送小消息，fontsrv 未就绪时重试（最多 tries 次，每次 yield 让出 CPU） */
static int fontsrv_send_retry(const void *msg, uint32_t size, int tries)
{
    if (!get_reply_port()) return -1;
    for (int i = 0; i < tries; i++) {
        if (mach_msg_send((void*)msg, size) == 0) return 0;
        sys_yield();
    }
    return -1;
}

/* 接收回执（阻塞重试，最多 tries 次） */
static int fontsrv_recv_reply(void *buf, uint32_t limit, int tries)
{
    for (int i = 0; i < tries; i++) {
        if (mach_msg_recv(buf, limit, g_reply_port) == 0) return 0;
        sys_yield();
    }
    return -1;
}

/* 查找或注册当前画布的 fontsrv handle（OOL 注册，持久映射） */
static uint32_t ensure_registered(void *va, int w, int h)
{
    for (int i = 0; i < SUI_FREG_MAX; i++)
        if (g_regs[i].used && g_regs[i].va == va) return g_regs[i].handle;

    if (!get_reply_port()) return 0;
    font_register_req_t req; memset(&req, 0, sizeof req);
    req.h.msgh_bits        = MACH_SEND_MSG | MACH_MSGH_BITS_OOL;
    req.h.msgh_size        = sizeof(req);
    req.h.msgh_remote_port = FONT_PORT;
    req.h.msgh_local_port  = g_reply_port;
    req.h.msgh_id          = FONT_MSG_REGISTER;
    req.ool.address = (uint64_t)va;
    req.ool.size   = (uint64_t)w * h * 4;
    req.buf_w = (uint32_t)w; req.buf_h = (uint32_t)h; req.pixel_format = 0;

    if (fontsrv_send_retry(&req, sizeof(req), 600) != 0) { g_fontsrv_ok = false; return 0; }

    font_register_done_t done;
    if (fontsrv_recv_reply(&done, sizeof(done), 600) != 0) { g_fontsrv_ok = false; return 0; }
    if (done.status != 0 || done.handle == 0) { g_fontsrv_ok = false; return 0; }

    for (int k = 0; k < SUI_FREG_MAX; k++) {
        if (!g_regs[k].used) {
            g_regs[k].used = true; g_regs[k].va = va;
            g_regs[k].handle = done.handle; g_regs[k].w = w; g_regs[k].h = h;
            break;
        }
    }
    g_fontsrv_ok = true;
    return done.handle;
}

int sui_font_init(void)
{
    sui_font_cpy(g_font_path, "/FONTS/RESOURCEHANROUNDEDCN-MEDIUM.TTF", sizeof g_font_path);
    g_fontsrv_ok = false;
    return 0;
}

void sui_font_set_default(const char *path)
{
    if (path) sui_font_cpy(g_font_path, path, sizeof g_font_path);
}

bool sui_font_ready(void) { return g_fontsrv_ok; }

int sui_font_draw(sui_canvas_t *c, int x, int y, const char *text, int size, uint32_t color)
{
    if (!c || !text || !c->pixels) return -1;
    uint32_t handle = ensure_registered(c->pixels, c->width, c->height);
    if (handle == 0) return -1;

    font_render_buf_req_t req; memset(&req, 0, sizeof req);
    req.h.msgh_bits        = MACH_SEND_MSG;
    req.h.msgh_size        = sizeof(req);
    req.h.msgh_remote_port = FONT_PORT;
    req.h.msgh_local_port  = g_reply_port;
    req.h.msgh_id          = FONT_MSG_RENDER_BUF;
    req.handle     = handle;
    req.x = (int32_t)x; req.y = (int32_t)y;
    req.pixel_size = (uint32_t)size;
    req.color      = color;
    req.clip_x = (int32_t)c->clip_x; req.clip_y = (int32_t)c->clip_y;
    req.clip_w = (int32_t)c->clip_w; req.clip_h = (int32_t)c->clip_h;
    req.unicode_mode = 0;
    sui_font_cpy(req.font_path, g_font_path[0] ? g_font_path
                                              : "/FONTS/RESOURCEHANROUNDEDCN-MEDIUM.TTF",
                 sizeof req.font_path);
    sui_font_cpy(req.text, text, FONT_TEXT_MAX);

    if (fontsrv_send_retry(&req, sizeof(req), 600) != 0) { g_fontsrv_ok = false; return -1; }

    font_render_done_t done;
    if (fontsrv_recv_reply(&done, sizeof(done), 600) != 0) { g_fontsrv_ok = false; return -1; }
    return (done.status == 0) ? 0 : -1;
}

int sui_font_measure(const char *text, int size)
{
    if (!text || size <= 0) return 0;
    if (!get_reply_port()) return -1;

    font_measure_req_t req; memset(&req, 0, sizeof req);
    req.h.msgh_bits        = MACH_SEND_MSG;
    req.h.msgh_size        = sizeof(req);
    req.h.msgh_remote_port = FONT_PORT;
    req.h.msgh_local_port  = g_reply_port;
    req.h.msgh_id          = FONT_MSG_MEASURE;
    req.pixel_size = (uint32_t)size;
    req.unicode_mode = 0;
    sui_font_cpy(req.font_path, g_font_path[0] ? g_font_path
                                              : "/FONTS/RESOURCEHANROUNDEDCN-MEDIUM.TTF",
                 sizeof req.font_path);
    sui_font_cpy(req.text, text, FONT_TEXT_MAX);

    if (fontsrv_send_retry(&req, sizeof(req), 600) != 0) { g_fontsrv_ok = false; return -1; }

    font_measure_done_t done;
    if (fontsrv_recv_reply(&done, sizeof(done), 600) != 0) { g_fontsrv_ok = false; return -1; }
    if (done.status != 0) { g_fontsrv_ok = false; return -1; }
    g_fontsrv_ok = true;
    return (int)done.width;
}

/* 窗口销毁时解除画布注册（由 libsui 在 sui_destroy_window 调用） */
void sui_font_unregister_all(void)
{
    for (int i = 0; i < SUI_FREG_MAX; i++) {
        if (!g_regs[i].used) continue;
        if (!get_reply_port()) { g_regs[i].used = false; continue; }
        font_unregister_req_t req; memset(&req, 0, sizeof req);
        req.h.msgh_bits        = MACH_SEND_MSG;
        req.h.msgh_size        = sizeof(req);
        req.h.msgh_remote_port = FONT_PORT;
        req.h.msgh_local_port  = g_reply_port;
        req.h.msgh_id          = FONT_MSG_UNREGISTER;
        req.handle = g_regs[i].handle;
        mach_msg_send(&req, sizeof(req));   /* 尽力解除，不等待回执 */
        g_regs[i].used = false; g_regs[i].va = NULL; g_regs[i].handle = 0;
    }
}
