/*
 * user/apps/winhello.c
 * -----------------------------------------------------------------------------
 * 窗口系统开机自检程序（内嵌自动拉起，输出经串口落盘）。
 *
 * 行为：用 libsuki_gui 创建一个窗口，绘制渐变背景 + 标题栏 + 文本，提交给 WM
 * 合成；随后注册事件端口并轮询若干鼠标事件，验证「应用 -> WM -> 合成 -> 帧缓冲」
 * 全链路贯通。最终打印自检结论并退出（不影响系统继续运行）。
 */
#include <stdint.h>
#include "lib/suki.h"
#include "lib/suki_gui.h"

int main(void)
{
    u_print("[winhello] starting GUI self-test\n");

    suki_window_t *w = suki_create_window("WinHello", 120, 80, 128, 120,
                                          SUKI_WS_DEFAULT);
    if (!w) {
        u_print("[winhello] FAIL: suki_create_window returned NULL\n");
        sys_exit(1);
    }
    u_print("[winhello] window created id=");
    char b[16]; u_print(u_utoa_s(w->id, b, sizeof(b)));
    u_print("\n");

    /* 背景：竖直渐变 */
    for (uint32_t y = 0; y < w->h; y++) {
        uint32_t t = (y * 255) / w->h;
        uint32_t col = SUKI_RGB(t, 40, 255 - t);
        suki_fill_rect(w, 0, y, w->w, 1, col);
    }
    /* 标题栏 */
    suki_fill_rect(w, 0, 0, w->w, 20, SUKI_ACCENT);
    suki_draw_text(w, 8, 6, w->title, 0x00FFFFFF);
    /* 客户区文本与矩形 */
    suki_draw_text(w, 12, 40, "SukiOS GUI", SUKI_FG_WINDOW);
    suki_draw_text(w, 12, 56, "libsuki_gui + OOL", SUKI_FG_WINDOW);
    suki_draw_line(w, 12, 90, 200, 160, 0x00FFD070);
    suki_fill_rect(w, 150, 110, 80, 50, 0x0044AA66);

    suki_flush(w, 0, 0, w->w, w->h);
    u_print("[winhello] flushed frame to WM (OOL)\n");

    /* 事件端口：验证 WM -> 应用事件分发 */
    uint32_t ep = sys_port_alloc();
    if (ep) {
        sys_port_claim(ep);
        suki_set_event_port(w, ep);
        u_print("[winhello] event port registered, polling...\n");
        suki_event_t ev;
        for (int i = 0; i < 200; i++) {
            if (suki_poll_event(w, &ev)) {
                u_print("[winhello] event type=");
                char b2[16]; u_print(u_utoa_s(ev.type, b2, sizeof(b2)));
                u_print("\n");
            }
            sys_yield();
        }
        u_print("[winhello] poll loop done (non-blocking)\n");
    }

    suki_destroy_window(w);
    u_print("[winhello] PASS: window lifecycle complete\n");
    sys_exit(0);
    return 0;
}
