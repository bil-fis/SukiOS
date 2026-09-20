/*
 * user/libsui/src/sui_widgets.c
 * -----------------------------------------------------------------------------
 * iSuki UI 控件集实现：标签 / 按钮 / 复选框 / 开关 / 滑块 / 进度条 / 卡片 / 输入框。
 *
 * 所有控件遵循规范 9：继承 sui_widget_t；颜色取自主题；支持焦点 / 禁用 / 按下态；
 * 仅通过 canvas 绘制原语渲染到离屏缓冲（增量由调用方/display_server 负责）。
 */
#include "sui.h"
#include <string.h>

static uint32_t sui__text_color(const sui_widget_t *w, uint32_t fallback)
{
    (void)fallback;
    if (w && w->bg_color) return 0x00000000u;  /* 调用方按背景决定 */
    return sui_theme()->text_primary;
}

/* =========================================================================
 * 标签（Label）
 * ====================================================================== */
static void label_draw(sui_widget_t *w, sui_canvas_t *c)
{
    sui_label_t *l = (sui_label_t *)w;
    uint32_t col = l->color ? l->color : sui_theme()->text_primary;
    int fs = l->font_size > 0 ? l->font_size : SUI_FONT_BODY;
    int wt = sui_canvas_measure_text(c, w->text, fs, l->weight);
    int tx = w->abs_x;
    int ty = w->abs_y + (w->h - 8 * ((fs + 5) / 12)) / 2;
    (void)wt;
    sui_canvas_draw_text(c, tx, ty, w->text, fs, l->weight, col);
}
static const sui_widget_vtable_t g_label_vt = { "Label", label_draw, NULL, NULL, NULL };

sui_label_t *sui_label_create(sui_widget_t *parent, const char *text)
{
    sui_label_t *l = (sui_label_t *)sui__alloc(sizeof(sui_label_t));
    if (!l) return NULL;
    memset(l, 0, sizeof(*l));
    l->base.vtable = &g_label_vt;
    l->base.focusable = false;
    l->font_size = SUI_FONT_BODY; l->weight = SUI_WEIGHT_REGULAR;
    sui__strcpy(l->base.text, text);
    int fs = l->font_size, scale = (fs + 5) / 12;
    l->base.w = (int)sui_strlen(text ? text : "") * 8 * scale + 4;
    l->base.h = 8 * scale + 4;
    l->base.corner_radius = SUI_RADIUS_NONE;
    sui_widget_add_child(parent, &l->base);
    return l;
}
void sui_label_set_font(sui_label_t *l, int size, int weight)
{
    if (!l) return;
    l->font_size = size; l->weight = weight;
    int scale = (size + 5) / 12;
    l->base.w = (int)sui_strlen(l->base.text) * 8 * scale + 4;
    l->base.h = 8 * scale + 4;
}

/* =========================================================================
 * 按钮（Button）
 * ====================================================================== */
static uint32_t button_bg(const sui_button_t *b, bool *white_text)
{
    const sui_theme_t *t = sui_theme();
    *white_text = false;
    switch (b->variant) {
        case SUI_BTN_PRIMARY:
            *white_text = true;
            if (b->base.pressed) return t->accent_pressed;
            if (b->base.hovered) return t->accent_hover;
            return t->accent;
        case SUI_BTN_DANGER:
            *white_text = true;
            if (b->base.pressed) return t->accent_pressed;
            if (b->base.hovered) return t->danger;
            return t->danger;
        case SUI_BTN_SECONDARY:
            if (b->base.pressed) return t->surface_active;
            if (b->base.hovered) return t->surface_hover;
            return t->surface;
        case SUI_BTN_GHOST:
        case SUI_BTN_ICON:
        default:
            if (b->base.pressed) return t->surface_active;
            if (b->base.hovered) return t->surface_hover;
            return 0x00000000u;
    }
}
static void button_draw(sui_widget_t *w, sui_canvas_t *c)
{
    sui_button_t *b = (sui_button_t *)w;
    bool white;
    uint32_t bg = button_bg(b, &white);
    if (b->variant == SUI_BTN_GHOST) {
        sui_canvas_fill_rounded_rect(c, w->abs_x, w->abs_y, w->w, w->h,
                                     w->corner_radius, 0x00000000u);
    } else {
        sui_canvas_fill_rounded_rect(c, w->abs_x, w->abs_y, w->w, w->h,
                                     w->corner_radius, bg);
        if (b->variant == SUI_BTN_SECONDARY)
            sui_canvas_stroke_rounded_rect(c, w->abs_x, w->abs_y, w->w, w->h,
                                           w->corner_radius, 1, sui_theme()->stroke);
    }
    uint32_t txt = white ? 0x00FFFFFFu : sui_theme()->text_primary;
    int fs = SUI_FONT_BODY;
    int scale = (fs + 5) / 12;
    int tw = sui_canvas_measure_text(c, w->text, fs, SUI_WEIGHT_MEDIUM);
    int tx = w->abs_x + (w->w - tw) / 2;
    int ty = w->abs_y + (w->h - 8 * scale) / 2;
    (void)scale;
    sui_canvas_draw_text(c, tx, ty, w->text, fs, SUI_WEIGHT_MEDIUM, txt);
}
static void button_on_event(sui_widget_t *w, const sui_event_t *ev)
{
    if (!w->enabled) return;
    sui_button_t *b = (sui_button_t *)w;
    if (ev->type == SUI_EVENT_CLICK && b->on_click) {
        b->on_click(w, b->user);
    }
}
static void button_measure(sui_widget_t *w)
{
    int fs = SUI_FONT_BODY, scale = (fs + 5) / 12;
    w->w = (int)sui_strlen(w->text) * 8 * scale + 24;
    w->h = (int)(w->tag ? w->tag : SUI_BTN_SIZE_MD);
}
static const sui_widget_vtable_t g_button_vt = {
    "Button", button_draw, button_measure, button_on_event, NULL
};

sui_button_t *sui_button_create(sui_widget_t *parent, sui_button_variant_t v,
                                sui_button_size_t s, const char *text)
{
    sui_button_t *b = (sui_button_t *)sui__alloc(sizeof(sui_button_t));
    if (!b) return NULL;
    memset(b, 0, sizeof(*b));
    b->base.vtable = &g_button_vt;
    b->variant = v; b->size = s;
    b->base.tag = (int)s;
    sui__strcpy(b->base.text, text);
    b->base.corner_radius = SUI_RADIUS_CTRL;
    if (v == SUI_BTN_ICON) { b->base.w = (int)s; b->base.h = (int)s; }
    else button_measure(&b->base);
    sui_widget_add_child(parent, &b->base);
    return b;
}
void sui_button_set_text(sui_button_t *b, const char *text)
{
    if (b) { sui__strcpy(b->base.text, text); button_measure(&b->base); }
}
void sui_button_on_click(sui_button_t *b, void (*fn)(sui_widget_t *, void *), void *user)
{
    if (b) { b->on_click = fn; b->user = user; }
}

/* =========================================================================
 * 复选框（Checkbox）
 * ====================================================================== */
static void checkbox_draw(sui_widget_t *w, sui_canvas_t *c)
{
    sui_checkbox_t *cb = (sui_checkbox_t *)w;
    const sui_theme_t *t = sui_theme();
    uint32_t box_color = (cb->checked || cb->indeterminate) ? t->accent : 0x00000000u;
    sui_canvas_stroke_rounded_rect(c, w->abs_x, w->abs_y, 16, 16, 4, 1,
                                   cb->checked ? t->accent : t->box_border);
    if (cb->checked || cb->indeterminate) {
        sui_canvas_fill_rounded_rect(c, w->abs_x + 2, w->abs_y + 2, 12, 12, 3, box_color);
        /* 勾选标记（对角） */
        sui_canvas_draw_line(c, w->abs_x + 4, w->abs_y + 9, w->abs_x + 7, w->abs_y + 12, 0x00FFFFFFu, 2);
        sui_canvas_draw_line(c, w->abs_x + 7, w->abs_y + 12, w->abs_x + 13, w->abs_y + 5, 0x00FFFFFFu, 2);
        if (cb->indeterminate)
            sui_canvas_draw_line(c, w->abs_x + 4, w->abs_y + 8, w->abs_x + 12, w->abs_y + 8, 0x00FFFFFFu, 2);
    }
    int fs = SUI_FONT_BODY, scale = (fs + 5) / 12;
    sui_canvas_draw_text(c, w->abs_x + 22, w->abs_y + (16 - 8 * scale) / 2,
                         w->text, fs, SUI_WEIGHT_REGULAR, t->text_primary);
}
static void checkbox_on_event(sui_widget_t *w, const sui_event_t *ev)
{
    if (!w->enabled) return;
    sui_checkbox_t *cb = (sui_checkbox_t *)w;
    if (ev->type == SUI_EVENT_CLICK) {
        cb->checked = !cb->checked; cb->indeterminate = false;
        if (cb->on_change) cb->on_change(w, cb->checked, cb->user);
    }
}
static const sui_widget_vtable_t g_checkbox_vt = { "Checkbox", checkbox_draw, NULL, checkbox_on_event, NULL };

sui_checkbox_t *sui_checkbox_create(sui_widget_t *parent, const char *label, bool checked)
{
    sui_checkbox_t *cb = (sui_checkbox_t *)sui__alloc(sizeof(sui_checkbox_t));
    if (!cb) return NULL;
    memset(cb, 0, sizeof(*cb));
    cb->base.vtable = &g_checkbox_vt;
    cb->checked = checked;
    sui__strcpy(cb->base.text, label);
    int fs = SUI_FONT_BODY, scale = (fs + 5) / 12;
    cb->base.w = 22 + (int)sui_strlen(label ? label : "") * 8 * scale + 4;
    cb->base.h = 22;
    cb->base.corner_radius = SUI_RADIUS_NONE;
    sui_widget_add_child(parent, &cb->base);
    return cb;
}
void sui_checkbox_set_checked(sui_checkbox_t *cb, bool checked)
{
    if (cb) { cb->checked = checked; cb->indeterminate = false; }
}
void sui_checkbox_on_change(sui_checkbox_t *cb, void (*fn)(sui_widget_t *, bool, void *), void *user)
{
    if (cb) { cb->on_change = fn; cb->user = user; }
}

/* =========================================================================
 * 开关（Switch）
 * ====================================================================== */
static void switch_draw(sui_widget_t *w, sui_canvas_t *c)
{
    sui_switch_t *s = (sui_switch_t *)w;
    int track_r = w->h / 2;
    sui_canvas_fill_rounded_rect(c, w->abs_x, w->abs_y, w->w, w->h, track_r,
                                 s->on ? sui_theme()->accent : sui_theme()->switch_off);
    int thumb = w->h - 4;
    int tx = s->on ? (w->abs_x + w->w - 2 - thumb) : (w->abs_x + 2);
    sui_canvas_fill_circle(c, tx + thumb / 2, w->abs_y + w->h / 2, thumb / 2, 0x00FFFFFFu);
}
static void switch_on_event(sui_widget_t *w, const sui_event_t *ev)
{
    if (!w->enabled) return;
    sui_switch_t *s = (sui_switch_t *)w;
    if (ev->type == SUI_EVENT_CLICK) {
        s->on = !s->on;
        if (s->on_change) s->on_change(w, s->on, s->user);
    }
}
static const sui_widget_vtable_t g_switch_vt = { "Switch", switch_draw, NULL, switch_on_event, NULL };

sui_switch_t *sui_switch_create(sui_widget_t *parent, bool on)
{
    sui_switch_t *s = (sui_switch_t *)sui__alloc(sizeof(sui_switch_t));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->base.vtable = &g_switch_vt;
    s->on = on;
    s->base.w = 44; s->base.h = 24;
    s->base.corner_radius = SUI_RADIUS_NONE;
    sui_widget_add_child(parent, &s->base);
    return s;
}
void sui_switch_set_on(sui_switch_t *s, bool on) { if (s) s->on = on; }
void sui_switch_on_change(sui_switch_t *s, void (*fn)(sui_widget_t *, bool, void *), void *user)
{
    if (s) { s->on_change = fn; s->user = user; }
}

/* =========================================================================
 * 滑块（Slider）
 * ====================================================================== */
static void slider_draw(sui_widget_t *w, sui_canvas_t *c)
{
    sui_slider_t *s = (sui_slider_t *)w;
    const sui_theme_t *t = sui_theme();
    int track_h = 6;
    int ty = w->abs_y + w->h / 2 - track_h / 2;
    int range = w->w - 16; if (range < 1) range = 1;
    int pos = (int)((int64_t)(s->value - s->min) * range / (s->max - s->min)) + 8;
    sui_canvas_fill_rounded_rect(c, w->abs_x, ty, w->w, track_h, track_h / 2, t->range_track);
    sui_canvas_fill_rounded_rect(c, w->abs_x, ty, pos - w->abs_x, track_h,
                                 track_h / 2, t->accent);
    sui_canvas_fill_circle(c, w->abs_x + pos, w->abs_y + w->h / 2, 8, t->range_thumb);
    sui_canvas_stroke_circle(c, w->abs_x + pos, w->abs_y + w->h / 2, 8, 1, t->stroke_soft);
    if (s->show_value) {
        char buf[16]; int len = 0; int v = s->value;
        if (v < 0) { buf[len++] = '-'; v = -v; }
        char tmp[12]; int n = 0; if (v == 0) tmp[n++] = '0';
        while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
        while (n) buf[len++] = tmp[--n];
        buf[len] = '\0';
        sui_canvas_draw_text(c, w->abs_x, w->abs_y + w->h + 2, buf, SUI_FONT_FOOTNOTE,
                             SUI_WEIGHT_REGULAR, t->text_secondary);
    }
}
static void slider_on_event(sui_widget_t *w, const sui_event_t *ev)
{
    if (!w->enabled) return;
    sui_slider_t *s = (sui_slider_t *)w;
    if ((ev->type == SUI_EVENT_MOUSE_DOWN || ev->type == SUI_EVENT_MOUSE_MOVE) && w->pressed) {
        int range = w->w - 16; if (range < 1) range = 1;
        int v = (ev->x - w->abs_x - 8);
        if (v < 0) v = 0;
        if (v > range) v = range;
        int val = s->min + (int)((int64_t)v * (s->max - s->min) / range);
        if (s->step > 1) val = s->min + ((val - s->min) / s->step) * s->step;
        if (val != s->value) {
            s->value = val;
            if (s->on_change) s->on_change(w, s->value, s->user);
        }
    }
}
static const sui_widget_vtable_t g_slider_vt = { "Slider", slider_draw, NULL, slider_on_event, NULL };

sui_slider_t *sui_slider_create(sui_widget_t *parent, int min, int max, int value)
{
    sui_slider_t *s = (sui_slider_t *)sui__alloc(sizeof(sui_slider_t));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->base.vtable = &g_slider_vt;
    s->min = min; s->max = max; s->value = value; s->step = 1;
    s->base.w = 160; s->base.h = 24;
    s->base.corner_radius = SUI_RADIUS_NONE;
    sui_widget_add_child(parent, &s->base);
    return s;
}
void sui_slider_set_value(sui_slider_t *s, int value) { if (s) s->value = value; }
void sui_slider_on_change(sui_slider_t *s, void (*fn)(sui_widget_t *, int, void *), void *user)
{
    if (s) { s->on_change = fn; s->user = user; }
}

/* =========================================================================
 * 进度条（Progress）
 * ====================================================================== */
static void progress_draw(sui_widget_t *w, sui_canvas_t *c)
{
    sui_progress_t *p = (sui_progress_t *)w;
    const sui_theme_t *t = sui_theme();
    int th = p->thickness > 0 ? p->thickness : 8;
    int r = th / 2;
    sui_canvas_fill_rounded_rect(c, w->abs_x, w->abs_y, w->w, th, r, t->range_track);
    int fill = (int)((int64_t)p->value * w->w / 100);
    if (fill > 0)
        sui_canvas_fill_rounded_rect(c, w->abs_x, w->abs_y, fill, th, r,
                                     p->color ? p->color : t->accent);
}
static const sui_widget_vtable_t g_progress_vt = { "Progress", progress_draw, NULL, NULL, NULL };

sui_progress_t *sui_progress_create(sui_widget_t *parent, int value)
{
    sui_progress_t *p = (sui_progress_t *)sui__alloc(sizeof(sui_progress_t));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    p->base.vtable = &g_progress_vt;
    p->value = value < 0 ? 0 : (value > 100 ? 100 : value);
    p->base.w = 200; p->base.h = 8;
    p->base.corner_radius = SUI_RADIUS_NONE;
    sui_widget_add_child(parent, &p->base);
    return p;
}
void sui_progress_set_value(sui_progress_t *p, int value)
{
    if (p) p->value = value < 0 ? 0 : (value > 100 ? 100 : value);
}

/* =========================================================================
 * 卡片（Card）
 * ====================================================================== */
static void card_draw(sui_widget_t *w, sui_canvas_t *c)
{
    sui_card_t *cd = (sui_card_t *)w;
    const sui_theme_t *t = sui_theme();
    sui_canvas_fill_rounded_rect(c, w->abs_x, w->abs_y, w->w, w->h, SUI_RADIUS_MD, t->card_bg);
    sui_canvas_stroke_rounded_rect(c, w->abs_x, w->abs_y, w->w, w->h, SUI_RADIUS_MD, 1, t->stroke_soft);
    int fs = SUI_FONT_SUBTITLE, scale = (fs + 5) / 12;
    sui_canvas_draw_text(c, w->abs_x + 12, w->abs_y + 12, w->text, fs, SUI_WEIGHT_SEMIBOLD, t->text_primary);
    sui_canvas_draw_text(c, w->abs_x + 12, w->abs_y + 12 + 8 * scale + 6, cd->body,
                         SUI_FONT_BODY, SUI_WEIGHT_REGULAR, t->text_secondary);
}
static const sui_widget_vtable_t g_card_vt = { "Card", card_draw, NULL, NULL, NULL };

sui_card_t *sui_card_create(sui_widget_t *parent, const char *title, const char *body)
{
    sui_card_t *cd = (sui_card_t *)sui__alloc(sizeof(sui_card_t));
    if (!cd) return NULL;
    memset(cd, 0, sizeof(*cd));
    cd->base.vtable = &g_card_vt;
    cd->base.focusable = false;
    sui__strcpy(cd->base.text, title);
    sui__strcpy(cd->body, body);
    cd->base.w = 280; cd->base.h = 110;
    cd->base.corner_radius = SUI_RADIUS_MD;
    sui_widget_add_child(parent, &cd->base);
    return cd;
}
void sui_card_set_body(sui_card_t *cd, const char *body) { if (cd) sui__strcpy(cd->body, body); }

/* =========================================================================
 * 输入框（Input，单行）
 * ====================================================================== */
static void input_draw(sui_widget_t *w, sui_canvas_t *c)
{
    sui_input_t *ip = (sui_input_t *)w;
    const sui_theme_t *t = sui_theme();
    sui_canvas_fill_rounded_rect(c, w->abs_x, w->abs_y, w->w, w->h, SUI_RADIUS_CTRL, t->input_bg);
    sui_canvas_stroke_rounded_rect(c, w->abs_x, w->abs_y, w->w, w->h, SUI_RADIUS_CTRL, 1,
                                   ip->base.focused ? t->accent : t->stroke);
    int fs = SUI_FONT_BODY, scale = (fs + 5) / 12;
    const char *txt = ip->text[0] ? ip->text : ip->placeholder;
    uint32_t col = ip->text[0] ? t->text_primary : t->text_tertiary;
    sui_canvas_draw_text(c, w->abs_x + 8, w->abs_y + (w->h - 8 * scale) / 2, txt, fs, SUI_WEIGHT_REGULAR, col);
    if (ip->base.focused && (ip->cursor < (int)sui_strlen(ip->text) || ip->text[0] == 0)) {
        int cx = w->abs_x + 8 + (int)sui_strlen(ip->text) * 8 * scale + 1;
        sui_canvas_draw_line(c, cx, w->abs_y + 6, cx, w->abs_y + w->h - 6, t->accent, 1);
    }
}
static void input_on_event(sui_widget_t *w, const sui_event_t *ev)
{
    sui_input_t *ip = (sui_input_t *)w;
    if (ev->type == SUI_EVENT_CLICK) {
        sui_widget_focus(w); return;
    }
    if (!w->enabled || !w->focused) return;
    if (ev->type == SUI_EVENT_KEY_CHAR && ev->ch) {
        size_t len = sui_strlen(ip->text);
        if (!ip->readonly && len < sizeof(ip->text) - 1 && ev->ch >= 0x20 && ev->ch < 0x7F) {
            ip->text[len] = ev->ch; ip->text[len + 1] = '\0'; ip->cursor = (int)len + 1;
            if (ip->on_change) ip->on_change(w, ip->text, ip->user);
        }
    } else if (ev->type == SUI_EVENT_KEY_DOWN) {
        if (ev->key == 0x0E && ip->cursor > 0) {  /* Backspace 近似 */
            size_t len = sui_strlen(ip->text);
            if (len) { ip->text[len - 1] = '\0'; ip->cursor = (int)len - 1;
                      if (ip->on_change) ip->on_change(w, ip->text, ip->user); }
        } else if (ev->key == 0x1C && ip->on_submit) {  /* Enter 近似 */
            ip->on_submit(w, ip->text, ip->user);
        }
    }
}
static const sui_widget_vtable_t g_input_vt = { "Input", input_draw, NULL, input_on_event, NULL };

sui_input_t *sui_input_create(sui_widget_t *parent, const char *placeholder)
{
    sui_input_t *ip = (sui_input_t *)sui__alloc(sizeof(sui_input_t));
    if (!ip) return NULL;
    memset(ip, 0, sizeof(*ip));
    ip->base.vtable = &g_input_vt;
    sui__strcpy(ip->placeholder, placeholder);
    ip->base.w = 240; ip->base.h = 28;
    ip->base.corner_radius = SUI_RADIUS_CTRL;
    sui_widget_add_child(parent, &ip->base);
    return ip;
}
void sui_input_set_text(sui_input_t *ip, const char *text) { if (ip) sui__strcpy(ip->text, text); }
const char *sui_input_text(const sui_input_t *ip) { return ip ? ip->text : ""; }

/* 避免未使用告警（sui__text_color 在部分路径被引用） */
static void sui__use_text_color(void) { (void)sui__text_color(NULL, 0); }
void sui_label_set_font_unused(void) { sui__use_text_color(); }
