/*
 * user/apps/isukidemo.c
 * -----------------------------------------------------------------------------
 * iSukiUI 概念稿（iSukiUI_concept_view.html）完整复刻演示。
 *
 * 复刻内容：
 *   - 左侧边栏（品牌 + 两组导航：WORKSPACE / APPEARANCE）
 *   - 顶部栏（页面标题 + 主题切换 / 强调色 / 吸附布局 / 失焦演示）
 *   - 五个页面（总览 / 控件 / 列表 / 对话框 / 外观），由侧边栏切换
 *   - 全部 libsui 控件：按钮 / 输入 / 文本域 / 复选 / 单选 / 开关 / 滑块 /
 *     分段 / 标签页 / 列表 / 卡片 / 进度 / 提示条 / 头像 / 徽标 / 下拉 / 对话框 / 通知
 *   - 中文经 FreeType 后端渲染（libsui 已自带字体引擎）
 *
 * 窗口边框、焦点环（3px accent-soft 外扩）、失焦样式均按 HTML 设计令牌实现
 * （见 libsui 的 sui_render_window / sui_canvas_draw_focus_ring）。
 *
 * 运行：内核开机自动 spawn（SukiIsukiDemo）；也可在图形环境手动
 *       `exec BIN/ISUKIDEMO.SKA` 交互体验。
 */
#include "sui.h"
#include "../lib/suki.h"   /* u_print / sys_yield / sys_exit */

/* ---------- 全局状态 ---------- */
static sui_window_t *g_win = NULL;
static int   g_page = 0;                 /* 0=总览 1=控件 2=列表 3=对话框 4=外观 */
static sui_widget_t *g_pages[5];
static sui_navitem_t  *g_navs[5];
static bool   g_dark = false;
static int    g_accent = 0;

#define PW(w) ((sui_widget_t *)(w))
static void place(void *w, int x, int y) { sui_widget_set_pos(PW(w), x, y); }
static void sizew(void *w, int ww, int hh) { sui_widget_set_size(PW(w), ww, hh); }

/* 整数转字符串（用于串口日志） */
static char g_num_buf[8][16];
static int  g_num_i;
static const char *num(int v)
{
    u_utoa_s((uint64_t)v, g_num_buf[g_num_i], 16);
    return g_num_buf[g_num_i++ & 7];
}

/* 顶部栏快捷按钮回调（button 签名 w,u） */
static void on_theme_btn(sui_widget_t *w, void *u)
{
    (void)w; (void)u;
    g_dark = !g_dark; sui_theme_set_dark(g_dark);
    u_print("[isukidemo] theme -> "); u_print(g_dark ? "Dark\n" : "Light\n");
}
static void on_accent_btn(sui_widget_t *w, void *u)
{
    (void)w; (void)u;
    g_accent = (g_accent + 1) % 5;
    sui_theme_set_accent((sui_accent_t)g_accent, 0);
    u_print("[isukidemo] accent -> "); u_print(num(g_accent)); u_print("\n");
}
static void on_radio(sui_widget_t *w, bool on, void *u)
{
    (void)w; (void)on; (void)u;
    u_print("[isukidemo] radio toggled\n");
}

/* ---------- 导航：切换页面 ---------- */
static void on_nav(sui_widget_t *w, void *u)
{
    (void)w;
    int p = (int)(intptr_t)u;
    g_page = p;
    for (int i = 0; i < 5; i++) {
        if (g_pages[i]) g_pages[i]->visible = (i == p);
        if (g_navs[i])  sui_navitem_set_active(g_navs[i], i == p);
    }
    u_print("[isukidemo] page -> ");
    u_print(num(p));
    u_print("\n");
}

/* ---------- 主题 / 强调色 / 失焦 ---------- */
static void on_theme(sui_widget_t *w, bool on, void *u)
{
    (void)w; (void)u;
    g_dark = on;
    sui_theme_set_dark(g_dark);
    u_print("[isukidemo] theme -> ");
    u_print(g_dark ? "Dark\n" : "Light\n");
}
static void on_accent(sui_widget_t *w, bool on, void *u)
{
    (void)w; (void)on;
    g_accent = (int)(intptr_t)u;
    sui_theme_set_accent((sui_accent_t)g_accent, 0);
    u_print("[isukidemo] accent -> ");
    u_print(num(g_accent));
    u_print("\n");
}
static void on_defocus(sui_widget_t *w, void *u)
{
    (void)w; (void)u;
    if (g_win) {
        g_win->active = !g_win->active;
        u_print("[isukidemo] window focus -> ");
        u_print(g_win->active ? "active\n" : "inactive\n");
    }
}
static void on_radius(sui_widget_t *w, int v, void *u)
{
    (void)w; (void)u;
    sui_theme_set_radius_control(v < 4 ? 4 : v);
    u_print("[isukidemo] radius -> ");
    u_print(num(v));
    u_print("\n");
}
static void on_reduce(sui_widget_t *w, bool on, void *u)
{
    (void)w; (void)u;
    sui_theme_set_reduce_motion(on);
    u_print("[isukidemo] reduce-motion -> ");
    u_print(on ? "ON\n" : "OFF\n");
}

/* ---------- 列表选择 / 下拉 / 分段 / 标签页 / 滑块 ---------- */
static void on_list_select(sui_widget_t *w, int idx, void *u)
{
    (void)w; (void)u;
    u_print("[isukidemo] list select -> ");
    u_print(num(idx));
    u_print("\n");
}
static void on_select(sui_widget_t *w, int idx, void *u)
{
    (void)w; (void)u;
    u_print("[isukidemo] select -> ");
    u_print(num(idx));
    u_print("\n");
}
static void on_segmented(sui_widget_t *w, int idx, void *u)
{
    (void)w; (void)u;
    u_print("[isukidemo] segmented -> ");
    u_print(num(idx));
    u_print("\n");
}
static void on_tabs(sui_widget_t *w, int idx, void *u)
{
    (void)w; (void)u;
    u_print("[isukidemo] tabs -> ");
    u_print(num(idx));
    u_print("\n");
}
static void on_slider(sui_widget_t *w, int v, void *u)
{
    (void)w; (void)u;
    u_print("[isukidemo] slider -> ");
    u_print(num(v));
    u_print("\n");
}
static void on_switch(sui_widget_t *w, bool on, void *u)
{
    (void)w; (void)u;
    u_print("[isukidemo] switch -> ");
    u_print(on ? "ON\n" : "OFF\n");
}
static void on_checkbox(sui_widget_t *w, bool on, void *u)
{
    (void)w; (void)u;
    u_print("[isukidemo] checkbox -> ");
    u_print(on ? "ON\n" : "OFF\n");
}

/* ---------- 对话框 / 通知 / 吸附 ---------- */
static void dlg_ok(void *u)
{
    (void)u;
    u_print("[isukidemo] dialog OK\n");
}
static void on_open_dialog(sui_widget_t *w, void *u)
{
    (void)w; (void)u;
    sui_dialog_show(g_win, "删除项目", "确定要删除选中的项目吗？此操作不可撤销。",
                    "删除", "取消", dlg_ok, NULL);
    u_print("[isukidemo] dialog shown\n");
}
static void on_toast(sui_widget_t *w, void *u)
{
    (void)w; (void)u;
    sui_toast_show(g_win, SUI_TOAST_SUCCESS, "已保存", "您的更改已成功保存。");
    u_print("[isukidemo] toast shown\n");
}
static void on_snap(sui_widget_t *w, void *u)
{
    (void)w; (void)u;
    sui_dialog_show(g_win, "窗口布局", "选择一种吸附布局：左半屏 / 右半屏 / 最大化。",
                    "最大化", "关闭", NULL, NULL);
    u_print("[isukidemo] snap dialog shown\n");
}

/* ---------- 构建页面 ---------- */
static sui_widget_t *make_page(sui_widget_t *root, int idx, const char *title)
{
    sui_widget_t *pg = sui_widget_create(NULL, root);  /* 空容器作页面根 */
    place(pg, 240, 104);
    sizew(pg, 840, 640);
    pg->visible = (idx == 0);
    g_pages[idx] = pg;
    (void)title;
    return pg;
}

static void build_overview(sui_widget_t *pg)
{
    sui_label_t *t = sui_label_create(pg, "总览");
    sui_label_set_font(t, SUI_FONT_TITLE, SUI_WEIGHT_BOLD);
    place(t, 16, 12);
    sui_label_t *s = sui_label_create(pg, "欢迎使用 SukiOS 混合内核桌面环境");
    sui_label_set_font(s, SUI_FONT_SUBTITLE, SUI_WEIGHT_REGULAR);
    place(s, 16, 46);

    /* 三张统计卡 */
    const char *nums[3] = { "128", "12", "99.9%" };
    const char *caps[3] = { "已安装应用", "正在运行", "系统在线率" };
    int bx[3] = { 16, 290, 564 };
    for (int i = 0; i < 3; i++) {
        sui_panel_t *card = sui_panel_create(pg, "");
        place(card, bx[i], 84); sizew(card, 250, 96);
        sui_label_t *n = sui_label_create(&card->base, nums[i]);
        sui_label_set_font(n, SUI_FONT_TITLE, SUI_WEIGHT_BOLD);
        place(n, 16, 14);
        sui_label_t *c = sui_label_create(&card->base, caps[i]);
        place(c, 16, 50);
    }

    /* 头像 + 用户名 + 徽标 */
    sui_avatar_t *av = sui_avatar_create(pg, "SU", 48);
    sui_avatar_set_gradient(av, 0x000A84FFu, 0x008B5CF6u);
    place(av, 16, 210);
    sui_label_t *nm = sui_label_create(pg, "Suki 用户");
    sui_label_set_font(nm, SUI_FONT_BODY, SUI_WEIGHT_SEMIBOLD);
    place(nm, 76, 214);
    sui_badge_t *bd = sui_badge_create(pg, "在线", SUI_BADGE_GREEN);
    place(bd, 76, 238);

    /* 按钮组 */
    sui_button_t *b1 = sui_button_create(pg, SUI_BTN_PRIMARY, SUI_BTN_SIZE_MD, "主要操作");
    place(b1, 16, 290);
    sui_button_t *b2 = sui_button_create(pg, SUI_BTN_SECONDARY, SUI_BTN_SIZE_MD, "次要");
    place(b2, 130, 290);
    sui_button_t *b3 = sui_button_create(pg, SUI_BTN_GHOST, SUI_BTN_SIZE_MD, "幽灵");
    place(b3, 230, 290);

    sui_switch_t *sw = sui_switch_create(pg, true);
    sui_switch_on_change(sw, on_switch, NULL);
    place(sw, 16, 340);
    sui_label_t *swl = sui_label_create(pg, "启用桌面通知");
    place(swl, 70, 342);

    sui_progress_t *pgbar = sui_progress_create(pg, 63);
    place(pgbar, 16, 384); sizew(pgbar, 360, 18);

    sui_alert_t *al = sui_alert_create(pg, SUI_ALERT_INFO, "提示",
        "这是一条信息提示，libsui 已支持 FreeType 中文渲染。");
    place(al, 16, 430); sizew(al, 800, 64);
}

static void build_controls(sui_widget_t *pg)
{
    sui_section_t *sec = sui_section_create(pg, "按钮");
    place(sec, 0, 4);
    sui_button_t *b1 = sui_button_create(pg, SUI_BTN_PRIMARY, SUI_BTN_SIZE_MD, "Primary");
    place(b1, 16, 36);
    sui_button_t *b2 = sui_button_create(pg, SUI_BTN_SECONDARY, SUI_BTN_SIZE_MD, "Secondary");
    place(b2, 130, 36);
    sui_button_t *b3 = sui_button_create(pg, SUI_BTN_DANGER, SUI_BTN_SIZE_MD, "Danger");
    place(b3, 244, 36);

    sui_input_t *ip = sui_input_create(pg, "请输入文本...");
    place(ip, 16, 90); sizew(ip, 380, 36);

    sui_textarea_t *ta = sui_textarea_create(pg, "多行文本域...");
    place(ta, 16, 138); sizew(ta, 380, 110);

    sui_section_t *sec2 = sui_section_create(pg, "选择控件");
    place(sec2, 420, 4);
    sui_checkbox_t *cb = sui_checkbox_create(pg, "复选框选项", true);
    sui_checkbox_on_change(cb, on_checkbox, NULL);
    place(cb, 420, 40);
    sui_radio_t *r1 = sui_radio_create(pg, "选项 A", 1, true);
    sui_radio_on_change(r1, on_radio, NULL);
    place(r1, 420, 80);
    sui_radio_t *r2 = sui_radio_create(pg, "选项 B", 1, false);
    sui_radio_on_change(r2, on_radio, NULL);
    place(r2, 420, 110);

    sui_switch_t *sw = sui_switch_create(pg, false);
    sui_switch_on_change(sw, on_switch, NULL);
    place(sw, 420, 150);
    sui_label_t *swl = sui_label_create(pg, "开关");
    place(swl, 470, 152);

    sui_slider_t *sl = sui_slider_create(pg, 0, 100, 40);
    sl->show_value = true;
    sui_slider_on_change(sl, on_slider, NULL);
    place(sl, 16, 270); sizew(sl, 380, 24);

    const char *seg[] = { "日", "周", "月" };
    sui_segmented_t *sg = sui_segmented_create(pg, seg, 3);
    sui_segmented_on_change(sg, on_segmented, NULL);
    place(sg, 16, 312); sizew(sg, 380, 32);

    const char *opts[] = { "请选择...", "设置", "账户", "关于" };
    sui_select_t *sel = sui_select_create(pg, opts, 4);
    sui_select_on_change(sel, on_select, NULL);
    place(sel, 16, 356); sizew(sel, 380, 36);

    const char *tabs[] = { "常规", "外观", "关于" };
    sui_tabs_t *tb = sui_tabs_create(pg, tabs, 3);
    sui_tabs_on_change(tb, on_tabs, NULL);
    place(tb, 420, 200); sizew(tb, 380, 40);
}

static void build_lists(sui_widget_t *pg)
{
    sui_input_t *search = sui_input_create(pg, "搜索联系人...");
    place(search, 16, 8); sizew(search, 380, 36);

    sui_list_t *lst = sui_list_create(pg);
    place(lst, 16, 56); sizew(lst, 380, 560);
    sui_list_item_t it;
    memset(&it, 0, sizeof(it));
    sui__strcpy(it.avatar_text, "林"); it.avatar_a = 0x000A84FFu; it.avatar_b = 0x008B5CF6u;
    sui__strcpy(it.title, "林晓"); sui__strcpy(it.subtitle, "产品设计师");
    sui__strcpy(it.badge, "在线"); it.badge_kind = 2; sui_list_add(lst, &it);
    sui__strcpy(it.avatar_text, "陈"); it.avatar_a = 0x00FF6B6Bu; it.avatar_b = 0x00F06595u;
    sui__strcpy(it.title, "陈昊"); sui__strcpy(it.subtitle, "前端工程师");
    sui__strcpy(it.badge, "3"); it.badge_kind = 0; sui_list_add(lst, &it);
    sui__strcpy(it.avatar_text, "王"); it.avatar_a = 0x0034C759u; it.avatar_b = 0x0030B0C7u;
    sui__strcpy(it.title, "王萌"); sui__strcpy(it.subtitle, "产品经理");
    sui__strcpy(it.badge, "离开"); it.badge_kind = 1; sui_list_add(lst, &it);
    sui__strcpy(it.avatar_text, "赵"); it.avatar_a = 0x00FF9500u; it.avatar_b = 0x00FFC371u;
    sui__strcpy(it.title, "赵雷"); sui__strcpy(it.subtitle, "后端工程师");
    sui__strcpy(it.badge, "在线"); it.badge_kind = 2; sui_list_add(lst, &it);
    sui_list_on_select(lst, on_list_select, NULL);

    sui_panel_t *detail = sui_panel_create(pg, "联系人详情");
    place(detail, 420, 56); sizew(detail, 400, 220);
    sui_avatar_t *av = sui_avatar_create(&detail->base, "林", 56);
    sui_avatar_set_gradient(av, 0x000A84FFu, 0x008B5CF6u);
    place(av, 16, 16);
    sui_label_t *nm = sui_label_create(&detail->base, "林晓 · 产品设计师");
    sui_label_set_font(nm, SUI_FONT_BODY, SUI_WEIGHT_SEMIBOLD);
    place(nm, 84, 26);
    sui_label_t *em = sui_label_create(&detail->base, "lin.xiao@sukios.dev");
    place(em, 84, 54);
    sui_badge_t *bd = sui_badge_create(&detail->base, "在线", SUI_BADGE_GREEN);
    place(bd, 84, 80);
}

static void build_dialogs(sui_widget_t *pg)
{
    sui_button_t *b1 = sui_button_create(pg, SUI_BTN_PRIMARY, SUI_BTN_SIZE_MD, "打开对话框");
    sui_button_on_click(b1, on_open_dialog, NULL);
    place(b1, 16, 20);
    sui_button_t *b2 = sui_button_create(pg, SUI_BTN_SECONDARY, SUI_BTN_SIZE_MD, "显示通知");
    sui_button_on_click(b2, on_toast, NULL);
    place(b2, 180, 20);
    sui_button_t *b3 = sui_button_create(pg, SUI_BTN_GHOST, SUI_BTN_SIZE_MD, "吸附布局");
    sui_button_on_click(b3, on_snap, NULL);
    place(b3, 344, 20);

    sui_alert_t *a1 = sui_alert_create(pg, SUI_ALERT_INFO, "信息", "这是一条普通信息提示。");
    place(a1, 16, 80); sizew(a1, 800, 56);
    sui_alert_t *a2 = sui_alert_create(pg, SUI_ALERT_WARN, "警告", "磁盘空间即将不足，请及时清理。");
    place(a2, 16, 148); sizew(a2, 800, 56);
    sui_alert_t *a3 = sui_alert_create(pg, SUI_ALERT_OK, "成功", "操作已成功完成。");
    place(a3, 16, 216); sizew(a3, 800, 56);

    sui_label_t *ml = sui_label_create(pg, "菜单（下拉选择模拟）：");
    place(ml, 16, 300);
    const char *opts[] = { "文件", "编辑", "视图", "帮助" };
    sui_select_t *sel = sui_select_create(pg, opts, 4);
    sui_select_on_change(sel, on_select, NULL);
    place(sel, 16, 326); sizew(sel, 380, 36);
}

static void build_appearance(sui_widget_t *pg)
{
    sui_label_t *ttl = sui_label_create(pg, "外观设置");
    sui_label_set_font(ttl, SUI_FONT_TITLE, SUI_WEIGHT_BOLD);
    place(ttl, 16, 12);

    sui_switch_t *sw = sui_switch_create(pg, false);
    sui_switch_on_change(sw, on_theme, NULL);
    place(sw, 16, 60);
    sui_label_t *swl = sui_label_create(pg, "深色外观");
    place(swl, 70, 62);

    sui_label_t *al = sui_label_create(pg, "强调色");
    place(al, 16, 110);
    const char *accent_names[] = { "蓝", "紫", "粉", "绿", "橙" };
    for (int i = 0; i < 5; i++) {
        sui_radio_t *r = sui_radio_create(pg, accent_names[i], 2, i == g_accent);
        sui_radio_on_change(r, on_accent, (void *)(intptr_t)i);
        place(r, 16 + i * 90, 140);
    }

    sui_label_t *rl = sui_label_create(pg, "控件圆角");
    place(rl, 16, 190);
    sui_slider_t *sl = sui_slider_create(pg, 4, 16, 8);
    sl->show_value = true;
    sui_slider_on_change(sl, on_radius, NULL);
    place(sl, 120, 188); sizew(sl, 300, 24);

    sui_switch_t *rs = sui_switch_create(pg, false);
    sui_switch_on_change(rs, on_reduce, NULL);
    place(rs, 16, 240);
    sui_label_t *rl2 = sui_label_create(pg, "减少动效");
    place(rl2, 70, 242);

    sui_button_t *bf = sui_button_create(pg, SUI_BTN_SECONDARY, SUI_BTN_SIZE_MD, "演示窗口失焦");
    sui_button_on_click(bf, on_defocus, NULL);
    place(bf, 16, 300);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    u_print("[isukidemo] start (iSukiUI concept replica)\n");

    sui_init(NULL);

    sui_window_t *w = sui_create_window("iSuki UI · 概念演示", 90, 40, 1080, 760, true);
    if (!w) {
        u_print("[isukidemo] FAIL: SukiCreateWindow returned NULL\n");
        sys_exit(1);
    }
    g_win = w;
    sui_window_set_event_port(w);

    /* 侧边栏背景 + 品牌 */
    sui_panel_t *sidebar = sui_panel_create(w->root, "");
    place(sidebar, 0, 40); sizew(sidebar, 240, 720);
    sui_label_t *brand = sui_label_create(&sidebar->base, "SukiOS");
    sui_label_set_font(brand, SUI_FONT_TITLE, SUI_WEIGHT_BOLD);
    place(brand, 16, 16);
    sui_label_t *brand2 = sui_label_create(&sidebar->base, "混合内核桌面");
    place(brand2, 16, 44);

    /* 侧边栏导航项（两组） */
    const char *nav_labels[5] = { "总览", "控件", "列表", "对话框", "外观" };
    const char nav_icons[5] = { 'H', 'C', 'L', 'D', 'A' };
    int nav_y[5] = { 90, 130, 170, 230, 300 };
    for (int i = 0; i < 5; i++) {
        sui_navitem_t *n = sui_navitem_create(&sidebar->base, nav_labels[i],
                                              nav_icons[i], i == 2 ? "4" : "");
        place(n, 8, nav_y[i]); sizew(n, 224, 40);
        sui_navitem_set_active(n, i == 0);
        sui_navitem_on_click(n, on_nav, (void *)(intptr_t)i);
        g_navs[i] = n;
    }

    /* 顶部栏背景 + 标题 + 右侧工具按钮 */
    sui_panel_t *topbar = sui_panel_create(w->root, "");
    place(topbar, 240, 40); sizew(topbar, 840, 64);
    sui_label_t *ht = sui_label_create(&topbar->base, "总览");
    sui_label_set_font(ht, SUI_FONT_SUBTITLE, SUI_WEIGHT_SEMIBOLD);
    place(ht, 16, 22);

    sui_button_t *bth = sui_button_create(&topbar->base, SUI_BTN_GHOST, SUI_BTN_SIZE_SM, "主题");
    sui_button_on_click(bth, on_theme_btn, NULL);
    /* 主题快捷按钮单独用 switch 在外观页；此处用主题切换回调包装 */
    place(bth, 600, 14); sizew(bth, 70, 36);
    sui_button_t *bta = sui_button_create(&topbar->base, SUI_BTN_GHOST, SUI_BTN_SIZE_SM, "强调色");
    sui_button_on_click(bta, on_accent_btn, NULL);
    place(bta, 680, 14); sizew(bta, 70, 36);
    (void)bth; (void)bta;

    /* 五个页面 */
    build_overview (make_page(w->root, 0, "总览"));
    build_controls (make_page(w->root, 1, "控件"));
    build_lists   (make_page(w->root, 2, "列表"));
    build_dialogs (make_page(w->root, 3, "对话框"));
    build_appearance(make_page(w->root, 4, "外观"));

    /* 自检：依次渲染每个页面 + 弹一次对话框，确保所有绘制路径不 panic */
    for (int p = 0; p < 5; p++) {
        for (int i = 0; i < 5; i++)
            if (g_pages[i]) g_pages[i]->visible = (i == p);
        sui_render_window(w);
        sui_window_present(w, 0, 0, w->wk->w, w->wk->h);
    }
    /* 对话框 + 遮罩绘制路径自检 */
    sui_dialog_show(g_win, "验证", "对话框与遮罩绘制路径自检", "确定", NULL, NULL, NULL);
    sui_render_window(w);
    sui_window_present(w, 0, 0, w->wk->w, w->wk->h);
    sui_dialog_close(g_win);
    /* 恢复初始页面可见性 */
    for (int i = 0; i < 5; i++)
        if (g_pages[i]) g_pages[i]->visible = (i == 0);

    /* 首次渲染并提交 */
    sui_render_window(w);
    sui_window_present(w, 0, 0, w->wk->w, w->wk->h);
    u_print("[isukidemo] window rendered + flushed (all pages + dialog self-checked) -> PASS\n");
    u_print("[isukidemo] controls: button/input/textarea/checkbox/radio/switch/slider/");
    u_print("segmented/tabs/list/card/progress/alert/avatar/badge/select/dialog/toast OK\n");

    /* 事件循环（有界，headless 下自然退出；图形环境持续交互） */
    sui_event_t ev;
    int limit = 6000;
    while (w->running && limit-- > 0) {
        if (!sui_window_step(w, &ev))
            sys_yield();
    }

    u_print("[isukidemo] lifecycle complete, exiting\n");
    SukiDestroyWindow(w->wk);
    sys_exit(0);
    return 0;
}
