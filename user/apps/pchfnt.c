/*
 * user/apps/pchfnt.c
 * -----------------------------------------------------------------------------
 * SukiOS 字体渲染命令行工具（外部程序）。
 *
 * 用法：
 *   pchfnt --font <path> --text "中文 Hello" [--size 48] [--x 100] [--y 100] [--color 0xFFFFFF]
 *   pchfnt --font <path> --text-unicode "0x4E2D 0x6587 72 0x48 0x69" ...
 *
 * 行为：经 FONT_PORT 把渲染请求发给常驻的 fontsrv，fontsrv 用 FreeType 把指定
 *       字体 + 文字光栅化为灰度位图并合成到帧缓冲（headless 下仅报告统计）。
 *       pchfnt 等待回执后打印结果并退出。
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

/* 简易十六进制/十进制解析（支持 0x 前缀） */
static uint32_t parse_u32(const char *s, uint32_t def)
{
    if (!s) return def;
    return (uint32_t)strtoul(s, NULL, 0);
}

int main(int argc, char **argv)
{
    const char *font_path = "/FONTS/RESOURCEHANROUNDEDCN-MEDIUM.TTF";
    const char *text = "Hello SukiOS";
    int unicode_mode = 0;
    uint32_t size = 48;
    int32_t x = 120, y = 140;
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

    font_render_req_t req;
    memset(&req, 0, sizeof req);
    req.h.msgh_bits = 0;
    req.h.msgh_size = sizeof(req);
    req.h.msgh_remote_port = FONT_PORT;
    req.h.msgh_local_port = APP_PORT;
    req.h.msgh_id = FONT_MSG_RENDER;
    req.h.msgh_reserved = 0;

    static uint32_t g_req_id = 1;
    req.req_id = g_req_id++;
    req.pixel_size = size;
    req.x = x; req.y = y;
    req.color = color;
    req.unicode_mode = unicode_mode ? 1 : 0;
    strncpy(req.font_path, font_path, sizeof req.font_path - 1);
    strncpy(req.text, text, sizeof req.text - 1);

    printf("[pchfnt] request: font=%s size=%u (%s) text=\"%s\"\n",
           font_path, size, unicode_mode ? "unicode" : "utf8", text);

    /* 重试发送：fontsrv 可能尚未完成端口认领（headless 自检场景） */
    int sent = 0;
    for (int i = 0; i < 300; i++) {
        if (mach_msg_send(&req, sizeof(req)) == 0) { sent = 1; break; }
        sys_yield();
    }
    if (!sent) {
        printf("[pchfnt] send to FONT_PORT failed (fontsrv not ready)\n");
        return 2;
    }

    /* 等待回执（带几次轮询避免永久阻塞） */
    static uint8_t rxb[sizeof(mach_msg_header_t) + sizeof(font_render_done_t) + 16];
    int got = 0;
    for (int tries = 0; tries < 200; tries++) {
        if (mach_msg_recv(rxb, sizeof rxb, APP_PORT) == 0) {
            mach_msg_header_t *h = (mach_msg_header_t*)rxb;
            if (h->msgh_id == FONT_MSG_RENDER_DONE) {
                font_render_done_t *d = (font_render_done_t*)rxb;
                printf("[pchfnt] done: status=%d glyphs=%d cache_hits=%d bbox=%dx%d\n",
                       d->status, d->glyphs, d->cache_hits, d->bbox_w, d->bbox_h);
                got = 1;
                break;
            }
        }
        sys_yield();
    }
    if (!got) {
        printf("[pchfnt] no reply from fontsrv (timeout)\n");
        return 3;
    }
    return 0;
}
