/*
 * user/display_server.c
 * -----------------------------------------------------------------------------
 * SukiOS 显示服务（Ring3，DISPLAY_PORT=3）—— 图形合成层。
 *
 * 职责（与内核分工）：
 *   - 内核 framebuffer.c 负责底层显存驱动与 fbcon 文本光栅化（已有）；
 *   - 显示服务负责「合成」：根据 configs/display.cfg 的配置（video_mode 开关、
 *     逻辑分辨率 1280x720 等）决定是否启用视频合成，并在帧缓冲上绘制桌面层。
 *
 * 工作流程：
 *   1. 认领 DISPLAY_PORT（A2 项 IPC 能力）。
 *   2. 调用 SYS_FRAMEBUFFER_MAP 取得帧缓冲用户态映射 + 实际/逻辑分辨率。
 *      - video_mode=on 且帧缓冲就绪：enabled=1，拿到用户虚拟地址，绘制合成桌面；
 *      - video_mode=off 或帧缓冲不可用：enabled=0，降级为纯文本转发（sys_debug_write），
 *        保持「纯文本输出」语义（不碰帧缓冲）。
 *   3. 进入消息循环：接收 SHELL/其它组件经 DISPLAY_PORT 发来的 DISP_MSG_TEXT，
 *      视频模式时绘制到客户区，文本模式时转发 sys_debug_write。
 *
 * 验证（headless）：服务在串口打印 [display] 模式/映射/分辨率等关键信息，
 * 经 -serial file 落盘后由 grep 判读；无头无法看像素，需人工在 QEMU 窗口确认
 * 桌面合成层（背景/边框/标题栏/分辨率标注）正确。
 */
#include "lib/suki.h"
#include <kernel/framebuffer.h>   /* fb_map_result_t：SYS_FRAMEBUFFER_MAP 返回结构 */
#include <stddef.h>

#ifndef DISP_MSG_TEXT
#define DISP_MSG_TEXT 1           /* shell 经 DISPLAY_PORT 发送的文本消息 id */
#endif

/* 桌面合成层调色板（XRGB 小端，与内核 FB_* 一致） */
#define DSP_BG      0x00101828u   /* 深色背景 */
#define DSP_BAR     0x00579BFEu   /* 标题栏蓝 */
#define DSP_BORDER  0x008BE9FDu   /* 边框青 */
#define DSP_TEXT    0x00FFFFFFu   /* 文本白 */
#define DSP_PANEL   0x001E2A3Au   /* 客户区面板 */

/* 在帧缓冲用户虚拟地址上画一个 32bpp 像素（fb 已是线性 XRGB） */
static inline void put_px(uint32_t *fb, uint32_t pitch, uint32_t x, uint32_t y,
                          uint32_t color)
{
    uint32_t *row = (uint32_t *)((uint8_t *)fb + (uint64_t)y * pitch);
    row[x] = color;
}

/* 填充矩形 */
static void fill_rect(uint32_t *fb, uint32_t pitch, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h, uint32_t color)
{
    for (uint32_t j = 0; j < h; j++)
        for (uint32_t i = 0; i < w; i++)
            put_px(fb, pitch, x + i, y + j, color);
}

/* 绘制桌面合成层：背景 + 边框 + 标题栏 + 分辨率指示矩形（按配置缩放映射）。 */
static void draw_desktop(uint32_t *fb, uint32_t pitch, uint32_t fw, uint32_t fh,
                         uint32_t cfg_w, uint32_t cfg_h)
{
    /* 整体背景 */
    fill_rect(fb, pitch, 0, 0, fw, fh, DSP_BG);

    /* 外边框（4px） */
    const uint32_t bw = 4;
    fill_rect(fb, pitch, 0, 0, fw, bw, DSP_BORDER);
    fill_rect(fb, pitch, 0, fh - bw, fw, bw, DSP_BORDER);
    fill_rect(fb, pitch, 0, 0, bw, fh, DSP_BORDER);
    fill_rect(fb, pitch, fw - bw, 0, bw, fh, DSP_BORDER);

    /* 顶部标题栏（高 24px） */
    fill_rect(fb, pitch, bw, bw, fw - 2 * bw, 24, DSP_BAR);

    /* 客户区面板（标题栏下方内缩 8px） */
    fill_rect(fb, pitch, bw + 8, bw + 24 + 8,
              fw - 2 * bw - 16, fh - (bw + 24 + 8) - bw - 8, DSP_PANEL);

    /* 分辨率指示矩形：把配置逻辑分辨率等比缩放进客户区中央，
     * 直观证明「配置文件的分辨率」真正影响了合成画面。 */
    uint32_t cw = fw - 2 * bw - 16;
    uint32_t ch = fh - (bw + 24 + 8) - bw - 8;
    uint32_t rx, ry, rw, rh;
    if (cfg_w * ch >= cfg_h * cw) {
        rw = cw * 3 / 4;
        rh = (uint32_t)((uint64_t)rw * cfg_h / cfg_w);
    } else {
        rh = ch * 3 / 4;
        rw = (uint32_t)((uint64_t)rh * cfg_w / cfg_h);
    }
    rx = bw + 8 + (cw - rw) / 2;
    ry = bw + 24 + 8 + (ch - rh) / 2;
    fill_rect(fb, pitch, rx, ry, rw, rh, 0x0050FA7Bu);  /* 绿：配置分辨率的「桌面」 */
}

int main(void)
{
    udbg_printf("[display] main entered\n");
    /* 1. 认领显示端口 */
    sys_port_claim(DISPLAY_PORT);
    udbg_printf("[display] port claimed\n");

    /* 2. 取得帧缓冲映射与配置 */
    fb_map_result_t res;
    long r = (long)suki_syscall6(SYS_FRAMEBUFFER_MAP, (uint64_t)&res, 0, 0, 0, 0, 0);
    udbg_printf("[display] SYS_FRAMEBUFFER_MAP returned ");
    udbg_printf(u_utoa_s((uint64_t)r, (char[12]){0}, 12));
    udbg_printf("\n");

    if (r == 0 && res.enabled) {
        uint32_t *fb = (uint32_t *)(uintptr_t)res.fb_user_va;
        /* 绘制桌面合成层 */
        draw_desktop(fb, res.pitch, res.width, res.height,
                     res.cfg_width, res.cfg_height);

        /* 关键状态：无条件打印，确保 make run 下也能确认显示服务就绪 */
        u_print("[display] VIDEO MODE active (desktop composited)\n");
        /* 详细诊断：受 CONFIG_DEBUG_SERIAL 开关控制（make run-dbg 才输出） */
        udbg_printf("[display] fb mapped @user_va=");
        udbg_printf(u_utoa_s((uint64_t)res.fb_user_va, (char[24]){0}, 24));
        udbg_printf(" phys=");
        udbg_printf(u_utoa_s(res.fb_phys, (char[24]){0}, 24));
        udbg_printf(" fb=");
        udbg_printf(u_utoa_s(res.width, (char[12]){0}, 12));
        udbg_printf("x");
        udbg_printf(u_utoa_s(res.height, (char[12]){0}, 12));
        udbg_printf(" pitch=");
        udbg_printf(u_utoa_s(res.pitch, (char[12]){0}, 12));
        udbg_printf(" cfg=");
        udbg_printf(u_utoa_s(res.cfg_width, (char[12]){0}, 12));
        udbg_printf("x");
        udbg_printf(u_utoa_s(res.cfg_height, (char[12]){0}, 12));
        udbg_printf("\n");

        /* 消息循环：接收文本并绘制到客户区（本期复用内核 fbcon 文本路径，
         * 显示服务仅做端口路由 + 已合成桌面层；后续里程碑接 GUI 文本栅格化） */
        static uint8_t mbuf[512];
        for (;;) {
            mach_msg_header_t *h = (mach_msg_header_t *)mbuf;
            if (mach_msg_recv(mbuf, sizeof(mbuf), DISPLAY_PORT) == 0) {
                if (h->msgh_id == DISP_MSG_TEXT) {
                    /* 文本模式由内核 fbcon 直接显示；显示服务转发一份到串口日志，
                     * 便于 headless 验证端口链路通畅 */
                    const char *txt = (const char *)(mbuf + sizeof(mach_msg_header_t));
                    u_print("[display] got text via DISPLAY_PORT: ");
                    u_print(txt);
                }
            }
            sys_yield();
        }
    } else {
        /* 纯文本回退：不碰帧缓冲，仅做端口路由转发 */
        u_print("[display] TEXT MODE: video_mode=off or no framebuffer; "
                "falling back to plain text output (no compositing)\n");

        static uint8_t mbuf[512];
        for (;;) {
            mach_msg_header_t *h = (mach_msg_header_t *)mbuf;
            if (mach_msg_recv(mbuf, sizeof(mbuf), DISPLAY_PORT) == 0) {
                if (h->msgh_id == DISP_MSG_TEXT) {
                    const char *txt = (const char *)(mbuf + sizeof(mach_msg_header_t));
                    sys_debug_write(txt, u_strlen(txt));  /* 转发到内核文本输出 */
                }
            }
            sys_yield();
        }
    }
}
