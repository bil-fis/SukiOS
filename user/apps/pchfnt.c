/*
 * user/apps/pchfnt.c
 * -----------------------------------------------------------------------------
 * SukiOS 字体渲染命令行工具（外部程序），用于验证字体服务 fontsrv 的离屏渲染链路。
 *
 * 用法：
 *   pchfnt --font <path> --text "中文 Hello" [--size 48] [--x 20] [--y 64] [--color 0xFFFFFF]
 *   pchfnt --font <path> --text-unicode "0x4E2D 0x6587 72 0x48 0x69" ...
 *
 * 行为：分配一块 xRGB32 离屏缓冲，经 FONT_MSG_REGISTER 把该缓冲（OOL 物理页）注册给
 *       常驻的 fontsrv（持久映射），再经 FONT_MSG_RENDER_BUF 让 fontsrv 把指定字体 + 文字
 *       光栅化为灰度位图并按 alpha 合成进该缓冲（不触碰屏幕帧缓冲）。等待回执后，扫描
 *       缓冲统计非透明像素数及覆盖矩形，确认离屏渲染确实发生，并打印结果。最后经
 *       FONT_MSG_UNREGISTER 解除映射并退出。
 *
 * 依赖：fontsrv 已常驻（shell 启动时会自动 suki_exec 拉起 ::BIN/FONTSRV.SKA）。
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "lib/suki.h"
#include <stdio.h>
#include "font_ipc.h"

static uint32_t parse_u32(const char *s, uint32_t def)
{
    if (!s) return def;
    return (uint32_t)strtoul(s, NULL, 0);
}

int main(int argc, char **argv)
{
    const char *font_path = "/FONTS/RESOURCEHANROUNDEDCN-MEDIUM.TTF";
    const char *text = "Hello SukiOS 中文字体";
    int unicode_mode = 0;
    uint32_t size = 48;
    int32_t x = 20, y = 64;
    uint32_t color = 0xFFFFFF;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--font") && i + 1 < argc) font_path = argv[++i];
        else if (!strcmp(argv[i], "--text") && i + 1 < argc) { text = argv[++i]; unicode_mode = 0; }
        else if (!strcmp(argv[i], "--text-unicode") && i + 1 < argc) { text = argv[++i]; unicode_mode = 1; }
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) size = parse_u32(argv[++i], size);
        else if (!strcmp(argv[i], "--x") && i + 1 < argc) x = (int32_t)parse_u32(argv[++i], (uint32_t)x);
        else if (!strcmp(argv[i], "--y") && i + 1 < argc) y = (int32_t)parse_u32(argv[++i], (uint32_t)y);
        else if (!strcmp(argv[i], "--color") && i + 1 < argc) color = parse_u32(argv[++i], color);
        else if (!strcmp(argv[i], "--help")) {
            printf("usage: pchfnt --font <path> --text \"text\" [--size N] [--x N] [--y N] [--color 0xRRGGBB]\n");
            printf("       pchfnt --font <path> --text-unicode \"0x4E2D 0x6587 72\" ...\n");
            return 0;
        }
    }

    if (sys_port_claim(APP_PORT) != 0) {
        printf("[pchfnt] cannot claim reply port\n");
        return 1;
    }

    /* 分配离屏缓冲（xRGB32），供 fontsrv 合成字形 */
    uint32_t bw = 1024, bh = 256;
    uint32_t *buf = (uint32_t *)sys_mmap((uint64_t)bw * bh * 4, 3);  /* PROT_READ|PROT_WRITE */
    if (!buf) { printf("[pchfnt] mmap offscreen buffer failed\n"); return 1; }
    memset(buf, 0, (size_t)bw * bh * 4);

    /* 注册缓冲（OOL） */
    font_register_req_t reg;
    memset(&reg, 0, sizeof reg);
    reg.h.msgh_bits        = MACH_SEND_MSG | MACH_MSGH_BITS_OOL;
    reg.h.msgh_size        = sizeof(reg);
    reg.h.msgh_remote_port = FONT_PORT;
    reg.h.msgh_local_port  = APP_PORT;
    reg.h.msgh_id          = FONT_MSG_REGISTER;
    reg.ool.address = (uint64_t)buf;
    reg.ool.size   = (uint64_t)bw * bh * 4;
    reg.buf_w = bw; reg.buf_h = bh; reg.pixel_format = 0;

    int sent = 0;
    for (int i = 0; i < 300; i++) {
        if (mach_msg_send(&reg, sizeof(reg)) == 0) { sent = 1; break; }
        sys_yield();
    }
    if (!sent) { printf("[pchfnt] register send to FONT_PORT failed (fontsrv not ready)\n"); return 2; }

    font_register_done_t rd;
    int got = 0;
    for (int i = 0; i < 300; i++) {
        if (mach_msg_recv(&rd, sizeof rd, APP_PORT) == 0 && rd.h.msgh_id == FONT_MSG_REGISTER_DONE) { got = 1; break; }
        sys_yield();
    }
    if (!got || rd.status != 0 || rd.handle == 0) {
        printf("[pchfnt] register failed (status=%d handle=%u)\n", got ? rd.status : -1, got ? rd.handle : 0);
        return 3;
    }
    uint32_t handle = rd.handle;
    printf("[pchfnt] buffer registered handle=%u (%ux%u)\n", handle, bw, bh);

    /* 渲染文本到离屏缓冲 */
    font_render_buf_req_t req;
    memset(&req, 0, sizeof req);
    req.h.msgh_bits        = MACH_SEND_MSG;
    req.h.msgh_size        = sizeof(req);
    req.h.msgh_remote_port = FONT_PORT;
    req.h.msgh_local_port  = APP_PORT;
    req.h.msgh_id          = FONT_MSG_RENDER_BUF;
    req.handle     = handle;
    req.x = x; req.y = y;
    req.pixel_size = size;
    req.color      = color;
    req.clip_x = 0; req.clip_y = 0; req.clip_w = (int32_t)bw; req.clip_h = (int32_t)bh;
    req.unicode_mode = unicode_mode ? 1 : 0;
    strncpy(req.font_path, font_path, sizeof req.font_path - 1);
    strncpy(req.text, text, sizeof req.text - 1);

    sent = 0;
    for (int i = 0; i < 300; i++) {
        if (mach_msg_send(&req, sizeof(req)) == 0) { sent = 1; break; }
        sys_yield();
    }
    if (!sent) { printf("[pchfnt] render send failed\n"); return 5; }

    font_render_done_t done;
    got = 0;
    for (int i = 0; i < 300; i++) {
        if (mach_msg_recv(&done, sizeof done, APP_PORT) == 0 && done.h.msgh_id == FONT_MSG_RENDER_DONE) { got = 1; break; }
        sys_yield();
    }
    if (!got) { printf("[pchfnt] no render reply\n"); return 6; }

    /* 扫描缓冲：统计非透明像素（验证离屏渲染确实发生） */
    uint32_t nonzero = 0;
    uint32_t minpx = bw, maxpx = 0, minpy = bh, maxpy = 0;
    for (uint32_t py = 0; py < bh; py++) {
        for (uint32_t px = 0; px < bw; px++) {
            uint32_t v = buf[py * bw + px];
            if ((v & 0xFF000000u) && (v & 0x00FFFFFFu)) {
                nonzero++;
                if (px < minpx) minpx = px;
                if (px > maxpx) maxpx = px;
                if (py < minpy) minpy = py;
                if (py > maxpy) maxpy = py;
            }
        }
    }

    printf("[pchfnt] done: status=%d glyphs=%d cache_hits=%d bbox=%dx%d\n",
           done.status, done.glyphs, done.cache_hits, done.bbox_w, done.bbox_h);
    printf("[pchfnt] offscreen buffer nonzero pixels=%u cover=[%u,%u]x[%u,%u]\n",
           nonzero, minpx, maxpx, minpy, maxpy);

    /* 解除注册（fontsrv 解映射物理页） */
    font_unregister_req_t un;
    memset(&un, 0, sizeof un);
    un.h.msgh_bits        = MACH_SEND_MSG;
    un.h.msgh_size        = sizeof(un);
    un.h.msgh_remote_port = FONT_PORT;
    un.h.msgh_local_port  = APP_PORT;
    un.h.msgh_id          = FONT_MSG_UNREGISTER;
    un.handle = handle;
    mach_msg_send(&un, sizeof un);

    if (done.status != 0) return 7;
    if (nonzero == 0) { printf("[pchfnt] WARN: no pixels rendered (check font/codepoints)\n"); return 8; }
    printf("[pchfnt] OK: offscreen render verified (%u pixels)\n", nonzero);
    return 0;
}
