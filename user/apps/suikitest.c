/*
 * user/apps/suikitest.c
 * -----------------------------------------------------------------------------
 * libsui 端到端冒烟测试：用原生控件库（libsui）创建一个带标题栏的窗口，
 * 叠放标签 / 按钮 / 复选框 / 开关 / 滑块 / 进度条 / 输入框 / 卡片，渲染并交由
 * display_server 合成。验证「应用 -> WM_PORT -> 显示服务增量合成 -> 帧缓冲」全链路，
 * 并经串口输出 [suikitest] PASS。
 *
 * 头文件精确路径：user/libsui/include/sui.h（经 -I user/libsui/include 引入）
 */
#include "sui.h"
#include "../lib/suki.h"   /* u_print / sys_yield / sys_exit */

static void on_button_click(sui_widget_t *w, void *ud)
{
    (void)w; (void)ud;
    u_print("[suikitest] button 'Primary' clicked\n");
}
static void on_switch_change(sui_widget_t *w, bool on, void *ud)
{
    (void)w; (void)ud;
    u_print("[suikitest] switch -> ");
    u_print(on ? "ON\n" : "OFF\n");
}
static void on_slider_change(sui_widget_t *w, int v, void *ud)
{
    (void)w; (void)ud;
    u_print("[suikitest] slider -> ");
    char b[16]; u_print(u_utoa_s((uint64_t)v, b, sizeof(b)));
    u_print("\n");
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    u_print("[suikitest] start (libsui UI library self-test)\n");

    sui_init(NULL);

    /* 注意：内核 OOL 单条上限已提升至 MACH_MSG_OOL_MAX_PAGES(2048 页 = 8MiB)，
     * 窗口离屏缓冲 w*h*4 可到 8MiB。660*440*4 = 1.16MiB 合法且匹配原布局。 */
    sui_window_t *w = sui_create_window("iSuki UI Demo", 180, 120, 660, 440, true);
    if (!w) {
        u_print("[suikitest] FAIL: SukiCreateWindow returned NULL\n");
        sys_exit(1);
    }
    sui_window_set_event_port(w);

    /* 标题区 */
    sui_label_t *title = sui_label_create(w->root, "iSuki UI 控件演示");
    sui_label_set_font(title, SUI_FONT_TITLE, SUI_WEIGHT_BOLD);
    sui_widget_set_pos(&title->base, 20, 14);
    sui_label_t *sub = sui_label_create(w->root, "基于 libsui 原生控件库 (theme=Light)");
    sui_label_set_font(sub, SUI_FONT_SUBTITLE, SUI_WEIGHT_REGULAR);
    sui_widget_set_pos(&sub->base, 20, 42);

    /* 按钮行 */
    sui_button_t *b1 = sui_button_create(w->root, SUI_BTN_PRIMARY, SUI_BTN_SIZE_MD, "Primary");
    sui_button_on_click(b1, on_button_click, NULL);
    sui_widget_set_pos(&b1->base, 20, 88);
    sui_button_t *b2 = sui_button_create(w->root, SUI_BTN_SECONDARY, SUI_BTN_SIZE_MD, "Secondary");
    sui_widget_set_pos(&b2->base, 130, 88);
    sui_button_t *b3 = sui_button_create(w->root, SUI_BTN_DANGER, SUI_BTN_SIZE_MD, "Danger");
    sui_widget_set_pos(&b3->base, 240, 88);

    /* 复选框 + 开关 */
    sui_checkbox_t *cb = sui_checkbox_create(w->root, "启用桌面通知", true);
    sui_widget_set_pos(&cb->base, 20, 140);
    sui_switch_t *sw = sui_switch_create(w->root, true);
    sui_switch_on_change(sw, on_switch_change, NULL);
    sui_widget_set_pos(&sw->base, 220, 140);
    sui_label_t *swl = sui_label_create(w->root, "深色外观");
    sui_widget_set_pos(&swl->base, 276, 142);

    /* 滑块 + 进度条 */
    sui_slider_t *sl = sui_slider_create(w->root, 0, 100, 42);
    sl->show_value = true;
    sui_widget_set_pos(&sl->base, 20, 184);
    sui_slider_on_change(sl, on_slider_change, NULL);
    sui_progress_t *pg = sui_progress_create(w->root, 63);
    sui_widget_set_pos(&pg->base, 200, 186);

    /* 输入框 */
    sui_input_t *ip = sui_input_create(w->root, "输入文字...");
    sui_widget_set_pos(&ip->base, 20, 236);

    /* 卡片 */
    sui_card_t *card = sui_card_create(w->root, "卡片标题",
                                       "这是一张卡片，用于承载结构化内容。");
    sui_widget_set_pos(&card->base, 300, 226);

    /* 首次渲染并提交 */
    sui_render_window(w);
    sui_window_present(w, 0, 0, w->wk->w, w->wk->h);
    u_print("[suikitest] window rendered + flushed -> PASS\n");

    /* 事件循环（无输入时让出 CPU；窗口关闭则退出）。
     * 为有界冒烟测试，最多轮询 4000 次后自然退出，避免 headless 下空转。 */
    sui_event_t ev;
    int limit = 4000;
    while (w->running && limit-- > 0) {
        if (!sui_window_step(w, &ev))
            sys_yield();
    }

    u_print("[suikitest] lifecycle complete, exiting\n");
    SukiDestroyWindow(w->wk);
    sys_exit(0);
    return 0;
}
