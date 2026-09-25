/*
 * user/libsui/src/sui_controls.c
 * -----------------------------------------------------------------------------
 * 扩展控件库：补全 iSuki UI 概念稿中的全部控件。
 *
 * 绘制风格严格参照 iSukiUI_concept_view.html 的 CSS 设计令牌（见设计规范
 * 文档第 5~9 节）：surface/input 背景、stroke 描边、accent 强调色、文本三级
 * 层级、圆角（control=8、window=12）、焦点环（3px accent-soft 外扩）。
 *
 * 控件结构通过 sui_widget_create_ex() 正确分配（扩展字段不再越界覆盖相邻控件）。
 * 所有绘制文本走 FreeType 后端（sui_font_draw），支持中文显示。
 */
#include "sui.h"
#include <string.h>

/* ===================== 工具 ===================== */
static void sui__foreach(sui_widget_t *root, void (*fn)(sui_widget_t *, void *), void *arg)
{
    if (!root) return;
    fn(root, arg);
    for (sui_widget_t *c = root->first_child; c; c = c->next_sibling)
        sui__foreach(c, fn, arg);
}
static sui_widget_t *sui__root(sui_widget_t *w)
{
    while (w && w->parent) w = w->parent;
    return w;
}
static int sui__strstr(const char *hay, const char *needle)
{
    if (!hay || !needle) return 0;
    int n = (int)sui_strlen(needle);
    if (!n) return 1;
    int m = (int)sui_strlen(hay);
    for (int i = 0; i + n <= m; i++) {
        int ok = 1;
        for (int j = 0; j < n; j++) if (hay[i + j] != needle[j]) { ok = 0; break; }
        if (ok) return 1;
    }
    return 0;
}
/* 仅描边（不擦除内部像素），四边直线近似圆角矩形边框 */
static void sui__border(sui_canvas_t *c, int x, int y, int w, int h, uint32_t col)
{
    (void)w;
    sui_canvas_draw_line(c, x, y, x + w - 1, y, col, 1);
    sui_canvas_draw_line(c, x, y + h - 1, x + w - 1, y + h - 1, col, 1);
    sui_canvas_draw_line(c, x, y, x, y + h - 1, col, 1);
    sui_canvas_draw_line(c, x + w - 1, y, x + w - 1, y + h - 1, col, 1);
}

/* ===================== Panel ===================== */
static void panel_draw(sui_widget_t *w, sui_canvas_t *c)
{
    const sui_theme_t *t = sui_theme();
    sui_canvas_fill_rounded_rect(c, w->abs_x, w->abs_y, w->w, w->h, w->corner_radius, t->card_bg);
    sui__border(c, w->abs_x, w->abs_y, w->w, w->h, t->stroke_soft);
    sui_panel_t *p = (sui_panel_t *)w;
    if (p->title[0]) {
        sui_canvas_draw_text(c, w->abs_x + 16, w->abs_y + 14, p->title,
                             SUI_FONT_TITLE, SUI_WEIGHT_SEMIBOLD, t->text_primary);
        sui_canvas_draw_line(c, w->abs_x + 16, w->abs_y + 40,
                             w->abs_x + w->w - 16, w->abs_y + 40, t->stroke_soft, 1);
    }
}
static const sui_widget_vtable_t g_panel_vt = { "panel", panel_draw, NULL, NULL, NULL };
sui_panel_t *sui_panel_create(sui_widget_t *parent, const char *title)
{
    sui_widget_t *w = sui_widget_create_ex(&g_panel_vt, parent, sizeof(sui_panel_t) - sizeof(sui_widget_t));
    if (!w) return NULL;
    sui_panel_t *p = (sui_panel_t *)w;
    sui__strcpy(p->title, title ? title : "");
    w->w = 320; w->h = 200;
    return p;
}

/* ===================== Section ===================== */
static void section_draw(sui_widget_t *w, sui_canvas_t *c)
{
    const sui_theme_t *t = sui_theme();
    sui_section_t *s = (sui_section_t *)w;
    if (s->title[0])
        sui_canvas_draw_text(c, w->abs_x + 16, w->abs_y + 8, s->title,
                             SUI_FONT_CAPTION, SUI_WEIGHT_SEMIBOLD, t->text_tertiary);
}
static const sui_widget_vtable_t g_section_vt = { "section", section_draw, NULL, NULL, NULL };
sui_section_t *sui_section_create(sui_widget_t *parent, const char *title)
{
    sui_widget_t *w = sui_widget_create_ex(&g_section_vt, parent, sizeof(sui_section_t) - sizeof(sui_widget_t));
    if (!w) return NULL;
    sui_section_t *s = (sui_section_t *)w;
    sui__strcpy(s->title, title ? title : "");
    w->w = 300; w->h = 260;
    return s;
}

/* ===================== NavItem ===================== */
static void navitem_draw(sui_widget_t *w, sui_canvas_t *c)
{
    const sui_theme_t *t = sui_theme();
    sui_navitem_t *n = (sui_navitem_t *)w;
    uint32_t bg = n->active ? t->surface_active : (w->hovered ? t->surface_hover : 0);
    if (bg) sui_canvas_fill_rounded_rect(c, w->abs_x, w->abs_y, w->w, w->h, 6, bg);
    if (n->active) sui_canvas_fill_rect(c, w->abs_x, w->abs_y + 6, 3, w->h - 12, t->accent);
    int base_y = w->abs_y + 12;
    if (n->icon)
        sui_canvas_draw_text(c, w->abs_x + 16, base_y, &n->icon, SUI_FONT_BODY, SUI_WEIGHT_MEDIUM, t->text_secondary);
    sui_canvas_draw_text(c, w->abs_x + 40, base_y, n->label, SUI_FONT_BODY, SUI_WEIGHT_MEDIUM,
                         n->active ? t->text_primary : t->text_secondary);
    if (n->count[0]) {
        int cw = sui_canvas_measure_text(c, n->count, SUI_FONT_CAPTION, 0) + 14;
        sui_canvas_fill_rounded_rect(c, w->abs_x + w->w - 12 - cw, w->abs_y + 10, cw, w->h - 20, 10, t->surface);
        sui_canvas_draw_text(c, w->abs_x + w->w - 6 - cw + 7, w->abs_y + 12, n->count,
                             SUI_FONT_CAPTION, 0, t->text_secondary);
    }
}
static bool navitem_event(sui_widget_t *w, const sui_event_t *ev)
{
    if (ev->type == SUI_EVENT_CLICK) {
        sui_navitem_t *n = (sui_navitem_t *)w;
        if (n->on_click) n->on_click(w, n->user);
    }
    return false;
}
static const sui_widget_vtable_t g_navitem_vt = { "navitem", navitem_draw, NULL, navitem_event, NULL };
sui_navitem_t *sui_navitem_create(sui_widget_t *parent, const char *label, char icon, const char *count)
{
    sui_widget_t *w = sui_widget_create_ex(&g_navitem_vt, parent, sizeof(sui_navitem_t) - sizeof(sui_widget_t));
    if (!w) return NULL;
    sui_navitem_t *n = (sui_navitem_t *)w;
    sui__strcpy(n->label, label ? label : "");
    n->icon = icon;
    sui__strcpy(n->count, count ? count : "");
    n->active = false; n->on_click = NULL; n->user = NULL;
    w->w = 210; w->h = 40;
    return n;
}
void sui_navitem_set_active(sui_navitem_t *n, bool a) { if (n) { ((sui_widget_t *)n)->dirty = true; n->active = a; } }
void sui_navitem_on_click(sui_navitem_t *n, void (*fn)(sui_widget_t *, void *), void *user)
{ if (n) { n->on_click = fn; n->user = user; } }

/* ===================== Radio ===================== */
typedef struct { sui_widget_t *w; int group; } uncheck_arg_t;
static void uncheck_fn(sui_widget_t *x, void *arg)
{
    uncheck_arg_t *ua = (uncheck_arg_t *)arg;
    if (x == ua->w) return;
    if (x->vtable && x->vtable->type_name && sui__strstr(x->vtable->type_name, "radio")) {
        sui_radio_t *r = (sui_radio_t *)x;
        if (r->group == ua->group) r->checked = false;
    }
}
static void radio_draw(sui_widget_t *w, sui_canvas_t *c)
{
    const sui_theme_t *t = sui_theme();
    sui_radio_t *r = (sui_radio_t *)w;
    int cy = w->abs_y + w->h / 2;
    sui_canvas_stroke_circle(c, w->abs_x + 10, cy, 9, 2, r->checked ? t->accent : t->box_border);
    if (r->checked) sui_canvas_fill_circle(c, w->abs_x + 10, cy, 5, t->accent);
    sui_canvas_draw_text(c, w->abs_x + 28, cy - 8, r->label, SUI_FONT_BODY, SUI_WEIGHT_MEDIUM, t->text_primary);
}
static bool radio_event(sui_widget_t *w, const sui_event_t *ev)
{
    if (ev->type == SUI_EVENT_CLICK) {
        sui_radio_t *r = (sui_radio_t *)w;
        if (!r->checked) {
            uncheck_arg_t ua = { w, r->group };
            sui__foreach(sui__root(w), uncheck_fn, &ua);
            r->checked = true;
            if (r->on_change) r->on_change(w, true, r->user);
        }
    }
    return false;
}
static const sui_widget_vtable_t g_radio_vt = { "radio", radio_draw, NULL, radio_event, NULL };
sui_radio_t *sui_radio_create(sui_widget_t *parent, const char *label, int group, bool checked)
{
    sui_widget_t *w = sui_widget_create_ex(&g_radio_vt, parent, sizeof(sui_radio_t) - sizeof(sui_widget_t));
    if (!w) return NULL;
    sui_radio_t *r = (sui_radio_t *)w;
    sui__strcpy(r->label, label ? label : "");
    r->group = group; r->checked = checked; r->on_change = NULL; r->user = NULL;
    w->w = 220; w->h = 28;
    return r;
}
void sui_radio_set_checked(sui_radio_t *r, bool c) { if (r) { ((sui_widget_t *)r)->dirty = true; r->checked = c; } }
void sui_radio_on_change(sui_radio_t *r, void (*fn)(sui_widget_t *, bool, void *), void *user)
{ if (r) { r->on_change = fn; r->user = user; } }

/* ===================== Segmented ===================== */
static void segmented_draw(sui_widget_t *w, sui_canvas_t *c)
{
    const sui_theme_t *t = sui_theme();
    sui_segmented_t *s = (sui_segmented_t *)w;
    sui_canvas_fill_rounded_rect(c, w->abs_x, w->abs_y, w->w, w->h, 8, t->surface);
    sui__border(c, w->abs_x, w->abs_y, w->w, w->h, t->stroke_soft);
    int segw = s->count ? w->w / s->count : w->w;
    for (int i = 0; i < s->count; i++) {
        int sx = w->abs_x + i * segw;
        if (i == s->selected) {
            sui_canvas_fill_rounded_rect(c, sx + 2, w->abs_y + 2, segw - 4, w->h - 4, 6, t->accent);
            sui_canvas_draw_text(c, sx, w->abs_y + w->h / 2 - 8, s->segments[i],
                                 SUI_FONT_BODY, SUI_WEIGHT_MEDIUM, 0x00FFFFFFu);
        } else {
            sui_canvas_draw_text(c, sx, w->abs_y + w->h / 2 - 8, s->segments[i],
                                 SUI_FONT_BODY, SUI_WEIGHT_MEDIUM, t->text_secondary);
        }
    }
}
static bool segmented_event(sui_widget_t *w, const sui_event_t *ev)
{
    if (ev->type == SUI_EVENT_CLICK) {
        sui_segmented_t *s = (sui_segmented_t *)w;
        int segw = s->count ? w->w / s->count : w->w;
        int idx = (ev->x - w->abs_x) / segw;
        if (idx >= 0 && idx < s->count && idx != s->selected) {
            s->selected = idx;
            if (s->on_change) s->on_change(w, idx, s->user);
        }
    }
    return false;
}
static const sui_widget_vtable_t g_segmented_vt = { "segmented", segmented_draw, NULL, segmented_event, NULL };
sui_segmented_t *sui_segmented_create(sui_widget_t *parent, const char **items, int count)
{
    sui_widget_t *w = sui_widget_create_ex(&g_segmented_vt, parent, sizeof(sui_segmented_t) - sizeof(sui_widget_t));
    if (!w) return NULL;
    sui_segmented_t *s = (sui_segmented_t *)w;
    s->count = count > 8 ? 8 : (count < 0 ? 0 : count);
    for (int i = 0; i < s->count; i++) sui__strcpy(s->segments[i], items[i] ? items[i] : "");
    s->selected = 0; s->on_change = NULL; s->user = NULL;
    w->w = 300; w->h = 32;
    return s;
}
void sui_segmented_set_selected(sui_segmented_t *s, int idx) { if (s && idx >= 0 && idx < s->count) s->selected = idx; }
void sui_segmented_on_change(sui_segmented_t *s, void (*fn)(sui_widget_t *, int, void *), void *user)
{ if (s) { s->on_change = fn; s->user = user; } }

/* ===================== Tabs ===================== */
static void tabs_draw(sui_widget_t *w, sui_canvas_t *c)
{
    const sui_theme_t *t = sui_theme();
    sui_tabs_t *s = (sui_tabs_t *)w;
    int step = s->count ? w->w / s->count : w->w;
    for (int i = 0; i < s->count; i++) {
        int tx = w->abs_x + i * step + 12;
        int ty = w->abs_y + 10;
        bool sel = (i == s->selected);
        sui_canvas_draw_text(c, tx, ty, s->tabs[i], SUI_FONT_BODY, SUI_WEIGHT_MEDIUM,
                             sel ? t->text_primary : t->text_secondary);
        if (sel) {
            int lw = sui_canvas_measure_text(c, s->tabs[i], SUI_FONT_BODY, 0);
            sui_canvas_fill_rect(c, tx, w->abs_y + w->h - 3, lw, 3, t->accent);
        }
    }
    sui_canvas_draw_line(c, w->abs_x, w->abs_y + w->h - 1, w->abs_x + w->w, w->abs_y + w->h - 1, t->stroke_soft, 1);
}
static bool tabs_event(sui_widget_t *w, const sui_event_t *ev)
{
    if (ev->type == SUI_EVENT_CLICK) {
        sui_tabs_t *s = (sui_tabs_t *)w;
        int step = s->count ? w->w / s->count : w->w;
        int idx = (ev->x - w->abs_x) / step;
        if (idx >= 0 && idx < s->count && idx != s->selected) {
            s->selected = idx;
            if (s->on_change) s->on_change(w, idx, s->user);
        }
    }
    return false;
}
static const sui_widget_vtable_t g_tabs_vt = { "tabs", tabs_draw, NULL, tabs_event, NULL };
sui_tabs_t *sui_tabs_create(sui_widget_t *parent, const char **items, int count)
{
    sui_widget_t *w = sui_widget_create_ex(&g_tabs_vt, parent, sizeof(sui_tabs_t) - sizeof(sui_widget_t));
    if (!w) return NULL;
    sui_tabs_t *s = (sui_tabs_t *)w;
    s->count = count > 8 ? 8 : (count < 0 ? 0 : count);
    for (int i = 0; i < s->count; i++) sui__strcpy(s->tabs[i], items[i] ? items[i] : "");
    s->selected = 0; s->on_change = NULL; s->user = NULL;
    w->w = 360; w->h = 40;
    return s;
}
void sui_tabs_set_selected(sui_tabs_t *t, int idx) { if (t && idx >= 0 && idx < t->count) t->selected = idx; }
void sui_tabs_on_change(sui_tabs_t *t, void (*fn)(sui_widget_t *, int, void *), void *user)
{ if (t) { t->on_change = fn; t->user = user; } }

/* ===================== List ===================== */
#define SUI_LIST_ITEM_H 56
static void list_draw(sui_widget_t *w, sui_canvas_t *c)
{
    const sui_theme_t *t = sui_theme();
    sui_list_t *l = (sui_list_t *)w;
    int vis = 0;
    for (int i = 0; i < l->count; i++) {
        sui_list_item_t *it = &l->items[i];
        if (l->filter[0] && !sui__strstr(it->title, l->filter) && !sui__strstr(it->subtitle, l->filter))
            continue;
        int iy = w->abs_y + vis * SUI_LIST_ITEM_H;
        if (i == l->selected)
            sui_canvas_fill_rounded_rect(c, w->abs_x, iy, w->w, SUI_LIST_ITEM_H - 4, 6, t->surface_active);
        int ay = iy + SUI_LIST_ITEM_H / 2;
        sui_canvas_fill_circle(c, w->abs_x + 28, ay, 16, it->avatar_a);
        sui_canvas_draw_text(c, w->abs_x + 22, ay - 6, it->avatar_text, SUI_FONT_BODY, SUI_WEIGHT_MEDIUM, 0x00FFFFFFu);
        sui_canvas_draw_text(c, w->abs_x + 56, iy + 12, it->title, SUI_FONT_BODY, SUI_WEIGHT_MEDIUM, t->text_primary);
        sui_canvas_draw_text(c, w->abs_x + 56, iy + 30, it->subtitle, SUI_FONT_CAPTION, 0, t->text_secondary);
        if (it->badge[0]) {
            int bw = sui_canvas_measure_text(c, it->badge, SUI_FONT_CAPTION, 0) + 12;
            uint32_t bc = it->badge_kind == 2 ? t->success : (it->badge_kind == 1 ? t->surface : t->accent);
            sui_canvas_fill_rounded_rect(c, w->abs_x + w->w - 12 - bw, iy + 18, bw, 20, 10, bc);
            sui_canvas_draw_text(c, w->abs_x + w->w - 6 - bw + 6, iy + 22, it->badge, SUI_FONT_CAPTION, 0, 0x00FFFFFFu);
        }
        vis++;
    }
}
static bool list_event(sui_widget_t *w, const sui_event_t *ev)
{
    if (ev->type == SUI_EVENT_CLICK) {
        sui_list_t *l = (sui_list_t *)w;
        int row = (ev->y - w->abs_y) / SUI_LIST_ITEM_H;
        if (row < 0) return false;
        int vis = 0, sel = -1;
        for (int i = 0; i < l->count; i++) {
            sui_list_item_t *it = &l->items[i];
            if (l->filter[0] && !sui__strstr(it->title, l->filter) && !sui__strstr(it->subtitle, l->filter))
                continue;
            if (vis == row) { sel = i; break; }
            vis++;
        }
        if (sel >= 0) { l->selected = sel; if (l->on_select) l->on_select(w, sel, l->user); }
    }
    return false;
}
static const sui_widget_vtable_t g_list_vt = { "list", list_draw, NULL, list_event, NULL };
sui_list_t *sui_list_create(sui_widget_t *parent)
{
    sui_widget_t *w = sui_widget_create_ex(&g_list_vt, parent, sizeof(sui_list_t) - sizeof(sui_widget_t));
    if (!w) return NULL;
    sui_list_t *l = (sui_list_t *)w;
    l->count = 0; l->selected = -1; l->filter[0] = 0; l->on_select = NULL; l->user = NULL;
    w->w = 340; w->h = 320;
    return l;
}
int sui_list_add(sui_list_t *l, const sui_list_item_t *item)
{
    if (!l || l->count >= 32 || !item) return -1;
    l->items[l->count] = *item;
    return l->count++;
}
void sui_list_set_filter(sui_list_t *l, const char *q) { if (l) sui__strcpy(l->filter, q ? q : ""); }
void sui_list_on_select(sui_list_t *l, void (*fn)(sui_widget_t *, int, void *), void *user)
{ if (l) { l->on_select = fn; l->user = user; } }

/* ===================== Alert ===================== */
static void alert_draw(sui_widget_t *w, sui_canvas_t *c)
{
    const sui_theme_t *t = sui_theme();
    sui_alert_t *a = (sui_alert_t *)w;
    uint32_t bar = a->kind == SUI_ALERT_WARN ? t->warning : (a->kind == SUI_ALERT_OK ? t->success : t->accent);
    sui_canvas_fill_rounded_rect(c, w->abs_x, w->abs_y, w->w, w->h, w->corner_radius, t->card_bg);
    sui__border(c, w->abs_x, w->abs_y, w->w, w->h, t->stroke_soft);
    sui_canvas_fill_rect(c, w->abs_x, w->abs_y + 10, 4, w->h - 20, bar);
    sui_canvas_draw_text(c, w->abs_x + 20, w->abs_y + 12, a->title, SUI_FONT_BODY, SUI_WEIGHT_SEMIBOLD, t->text_primary);
    sui_canvas_draw_text(c, w->abs_x + 20, w->abs_y + 34, a->body, SUI_FONT_CAPTION, 0, t->text_secondary);
}
static const sui_widget_vtable_t g_alert_vt = { "alert", alert_draw, NULL, NULL, NULL };
sui_alert_t *sui_alert_create(sui_widget_t *parent, sui_alert_kind_t kind, const char *title, const char *body)
{
    sui_widget_t *w = sui_widget_create_ex(&g_alert_vt, parent, sizeof(sui_alert_t) - sizeof(sui_widget_t));
    if (!w) return NULL;
    sui_alert_t *a = (sui_alert_t *)w;
    a->kind = kind;
    sui__strcpy(a->title, title ? title : "");
    sui__strcpy(a->body, body ? body : "");
    w->w = 360; w->h = 64;
    return a;
}

/* ===================== Avatar ===================== */
static void avatar_draw(sui_widget_t *w, sui_canvas_t *c)
{
    sui_avatar_t *a = (sui_avatar_t *)w;
    int r = a->size / 2, cx = w->abs_x + r, cy = w->abs_y + r;
    uint8_t ar = (a->grad_a >> 16) & 0xFF, ag = (a->grad_a >> 8) & 0xFF, ab = a->grad_a & 0xFF;
    uint8_t br = (a->grad_b >> 16) & 0xFF, bg = (a->grad_b >> 8) & 0xFF, bb = a->grad_b & 0xFF;
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++) {
            if (x * x + y * y > r * r) continue;
            int px = cx + x, py = cy + y;
            if (px < 0 || py < 0 || px >= c->width || py >= c->height) continue;
            int f = (r > 0) ? (y + r) * 256 / (2 * r) : 0;
            uint8_t rr = (uint8_t)(ar + (br - ar) * f / 256);
            uint8_t gg = (uint8_t)(ag + (bg - ag) * f / 256);
            uint8_t b2 = (uint8_t)(ab + (bb - ab) * f / 256);
            c->pixels[(uint64_t)py * c->width + px] = (0xFFu << 24) | (rr << 16) | (gg << 8) | b2;
        }
    sui_canvas_draw_text(c, cx - 4, cy - 8, a->text, SUI_FONT_BODY, SUI_WEIGHT_SEMIBOLD, 0x00FFFFFFu);
}
static const sui_widget_vtable_t g_avatar_vt = { "avatar", avatar_draw, NULL, NULL, NULL };
sui_avatar_t *sui_avatar_create(sui_widget_t *parent, const char *text, int size)
{
    sui_widget_t *w = sui_widget_create_ex(&g_avatar_vt, parent, sizeof(sui_avatar_t) - sizeof(sui_widget_t));
    if (!w) return NULL;
    sui_avatar_t *a = (sui_avatar_t *)w;
    sui__strcpy(a->text, text ? text : "");
    a->size = size; a->grad_a = 0x00FF6B6B; a->grad_b = 0x008B5CF6;
    w->w = size; w->h = size;
    return a;
}
void sui_avatar_set_gradient(sui_avatar_t *a, uint32_t ga, uint32_t gb)
{ if (a) { a->grad_a = ga; a->grad_b = gb; } }

/* ===================== Badge ===================== */
static void badge_draw(sui_widget_t *w, sui_canvas_t *c)
{
    const sui_theme_t *t = sui_theme();
    sui_badge_t *b = (sui_badge_t *)w;
    uint32_t bg = b->kind == SUI_BADGE_GREEN ? t->success : (b->kind == SUI_BADGE_GRAY ? t->surface : t->accent);
    sui_canvas_fill_rounded_rect(c, w->abs_x, w->abs_y, w->w, w->h, w->h / 2, bg);
    int tw = sui_canvas_measure_text(c, b->text, SUI_FONT_CAPTION, 0);
    sui_canvas_draw_text(c, w->abs_x + (w->w - tw) / 2, w->abs_y + w->h / 2 - 6, b->text, SUI_FONT_CAPTION, 0, 0x00FFFFFFu);
}
static const sui_widget_vtable_t g_badge_vt = { "badge", badge_draw, NULL, NULL, NULL };
sui_badge_t *sui_badge_create(sui_widget_t *parent, const char *text, sui_badge_kind_t kind)
{
    sui_widget_t *w = sui_widget_create_ex(&g_badge_vt, parent, sizeof(sui_badge_t) - sizeof(sui_widget_t));
    if (!w) return NULL;
    sui_badge_t *b = (sui_badge_t *)w;
    sui__strcpy(b->text, text ? text : "");
    b->kind = kind;
    int tw = (int)sui_strlen(b->text) * 8 + 16;
    w->w = tw < 28 ? 28 : tw; w->h = 18;
    return b;
}

/* ===================== TextArea ===================== */
static void textarea_draw(sui_widget_t *w, sui_canvas_t *c)
{
    const sui_theme_t *t = sui_theme();
    sui_textarea_t *ta = (sui_textarea_t *)w;
    sui_canvas_fill_rounded_rect(c, w->abs_x, w->abs_y, w->w, w->h, w->corner_radius, t->input_bg);
    sui__border(c, w->abs_x, w->abs_y, w->w, w->h,
                w->focused ? t->accent : t->box_border);
    int lx = w->abs_x + 10, ly = w->abs_y + 10;
    if (ta->text[0] == 0) {
        sui_canvas_draw_text(c, lx, ly, ta->placeholder, SUI_FONT_BODY, 0, t->text_tertiary);
        return;
    }
    int line = 0, cx = lx;
    for (int i = 0; ta->text[i]; i++) {
        char ch = ta->text[i];
        if (ch == '\n') { line++; cx = lx; continue; }
        if (cx > w->abs_x + w->w - 14) { line++; cx = lx; }
        sui_canvas_draw_text(c, cx, ly + line * 18, &ch, SUI_FONT_BODY, 0, t->text_primary);
        cx += sui_canvas_measure_text(c, &ch, SUI_FONT_BODY, 0);
    }
    if (w->focused)
        sui_canvas_draw_line(c, cx, ly + line * 18, cx, ly + line * 18 + 16, t->text_primary, 1);
}
static bool textarea_event(sui_widget_t *w, const sui_event_t *ev)
{
    sui_textarea_t *ta = (sui_textarea_t *)w;
    if (ev->type == SUI_EVENT_CLICK) { sui_widget_focus(w); return false; }
    if (ev->type == SUI_EVENT_KEY_CHAR) {
        int len = (int)sui_strlen(ta->text);
        if (len < 511 && ev->key >= 0x20 && ev->key < 0x7F) {
            ta->text[len] = (char)ev->key; ta->text[len + 1] = 0;
        }
        return false;
    }
    if (ev->type == SUI_EVENT_KEY_DOWN) {
        if (ev->key == 0x0E) { int len = (int)sui_strlen(ta->text); if (len > 0) ta->text[len - 1] = 0; }
        return false;
    }
    return false;
}
static const sui_widget_vtable_t g_textarea_vt = { "textarea", textarea_draw, NULL, textarea_event, NULL };
sui_textarea_t *sui_textarea_create(sui_widget_t *parent, const char *placeholder)
{
    sui_widget_t *w = sui_widget_create_ex(&g_textarea_vt, parent, sizeof(sui_textarea_t) - sizeof(sui_widget_t));
    if (!w) return NULL;
    sui_textarea_t *ta = (sui_textarea_t *)w;
    ta->text[0] = 0; ta->cursor = 0;
    sui__strcpy(ta->placeholder, placeholder ? placeholder : "");
    ta->on_change = NULL; ta->user = NULL;
    w->w = 320; w->h = 120; w->focusable = true;
    return ta;
}
void sui_textarea_set_text(sui_textarea_t *t, const char *text) { if (t) sui__strcpy(t->text, text ? text : ""); }
const char *sui_textarea_text(const sui_textarea_t *t) { return t ? t->text : ""; }

/* ===================== Select ===================== */
typedef struct { sui_select_t *sel; int idx; } opt_ctx_t;

static void opt_draw(sui_widget_t *w, sui_canvas_t *c)
{
    const sui_theme_t *t = sui_theme();
    opt_ctx_t *o = (opt_ctx_t *)w->user_data;
    sui_select_t *s = o->sel;
    sui_canvas_fill_rect(c, w->abs_x, w->abs_y, w->w, w->h, w->hovered ? t->surface_hover : t->menu_bg);
    sui_canvas_draw_text(c, w->abs_x + 10, w->abs_y + w->h / 2 - 8, s->options[o->idx], SUI_FONT_BODY, 0, t->text_primary);
}
static bool opt_event(sui_widget_t *w, const sui_event_t *ev)
{
    if (ev->type == SUI_EVENT_CLICK) {
        opt_ctx_t *o = (opt_ctx_t *)w->user_data;
        o->sel->selected = o->idx;
        o->sel->open = false;
        while (o->sel->base.first_child) sui_widget_destroy(o->sel->base.first_child);
        sui_event_stop(ev);
        if (o->sel->on_change) o->sel->on_change(&o->sel->base, o->idx, o->sel->user);
    }
    return false;
}
static const sui_widget_vtable_t g_opt_vt = { "opt", opt_draw, NULL, opt_event, NULL };

static void select_draw(sui_widget_t *w, sui_canvas_t *c)
{
    const sui_theme_t *t = sui_theme();
    sui_select_t *s = (sui_select_t *)w;
    sui_canvas_fill_rounded_rect(c, w->abs_x, w->abs_y, w->w, w->h, w->corner_radius, t->input_bg);
    sui__border(c, w->abs_x, w->abs_y, w->w, w->h,
                (w->focused || s->open) ? t->accent : t->box_border);
    const char *cur = (s->selected >= 0 && s->selected < s->count) ? s->options[s->selected] : "";
    sui_canvas_draw_text(c, w->abs_x + 10, w->abs_y + w->h / 2 - 8, cur, SUI_FONT_BODY, 0, t->text_primary);
    sui_canvas_draw_text(c, w->abs_x + w->w - 20, w->abs_y + w->h / 2 - 8, s->open ? "▲" : "▼",
                         SUI_FONT_BODY, 0, t->text_secondary);
}
static bool select_event(sui_widget_t *w, const sui_event_t *ev)
{
    sui_select_t *s = (sui_select_t *)w;
    if (ev->type == SUI_EVENT_CLICK) {
        if (!s->open) {
            s->open = true;
            for (int i = 0; i < s->count; i++) {
                sui_widget_t *opt = sui_widget_create(&g_opt_vt, w);
                opt->x = 0; opt->y = (i + 1) * w->h;
                opt->w = w->w; opt->h = w->h;
                opt_ctx_t *o = (opt_ctx_t *)sui__alloc(sizeof(opt_ctx_t));
                o->sel = s; o->idx = i; opt->user_data = o;
            }
        } else {
            s->open = false;
            while (w->first_child) sui_widget_destroy(w->first_child);
        }
    }
    return false;
}
static const sui_widget_vtable_t g_select_vt = { "select", select_draw, NULL, select_event, NULL };
sui_select_t *sui_select_create(sui_widget_t *parent, const char **opts, int count)
{
    sui_widget_t *w = sui_widget_create_ex(&g_select_vt, parent, sizeof(sui_select_t) - sizeof(sui_widget_t));
    if (!w) return NULL;
    sui_select_t *s = (sui_select_t *)w;
    s->count = count > 12 ? 12 : (count < 0 ? 0 : count);
    s->selected = -1; s->open = false; s->on_change = NULL; s->user = NULL;
    for (int i = 0; i < s->count; i++) sui__strcpy(s->options[i], opts[i] ? opts[i] : "");
    w->w = 240; w->h = 36;
    return s;
}
void sui_select_set_selected(sui_select_t *s, int idx) { if (s && idx >= 0 && idx < s->count) s->selected = idx; }
void sui_select_on_change(sui_select_t *s, void (*fn)(sui_widget_t *, int, void *), void *user)
{ if (s) { s->on_change = fn; s->user = user; } }

/* ===================== 浮层 / Toast / Dialog ===================== */
sui_widget_t *sui_window_overlay(sui_window_t *win) { return win ? win->overlay : NULL; }
void sui_overlay_clear(sui_widget_t *overlay)
{
    if (!overlay) return;
    while (overlay->first_child) sui_widget_destroy(overlay->first_child);
    overlay->visible = false;
}

/* ---- Toast ---- */
typedef struct { int kind; char title[64]; char body[128]; int ttl; } toast_t;
static toast_t g_toasts[4];
static int g_ntoast = 0;

void sui_toast_show(sui_window_t *win, sui_toast_kind_t kind, const char *title, const char *body)
{
    (void)win;
    if (g_ntoast >= 4) return;
    toast_t *tt = &g_toasts[g_ntoast++];
    tt->kind = kind;
    sui__strcpy(tt->title, title ? title : "");
    sui__strcpy(tt->body, body ? body : "");
    tt->ttl = 240;
}
void sui_toast_paint(sui_window_t *win, sui_canvas_t *c)
{
    (void)win;
    int y = c->height - 24;
    for (int i = 0; i < g_ntoast; i++) {
        toast_t *tt = &g_toasts[i];
        int th = 56, tw = 340, tx = (c->width - tw) / 2;
        y -= th + 10;
        uint32_t col = tt->kind == SUI_TOAST_SUCCESS ? 0x002E7D32u
                      : (tt->kind == SUI_TOAST_WARN ? 0x009C6B00u : 0x002F6FDBu);
        sui_canvas_fill_rounded_rect(c, tx, y, tw, th, 10, col);
        sui_canvas_draw_text(c, tx + 16, y + 10, tt->title, SUI_FONT_BODY, SUI_WEIGHT_SEMIBOLD, 0x00FFFFFFu);
        sui_canvas_draw_text(c, tx + 16, y + 30, tt->body, SUI_FONT_CAPTION, 0, 0x00E0E0E0u);
        tt->ttl--;
        if (tt->ttl <= 0) {
            for (int j = i; j < g_ntoast - 1; j++) g_toasts[j] = g_toasts[j + 1];
            g_ntoast--; i--;
        }
    }
}

/* ---- Dialog ---- */
typedef struct { void (*on_ok)(void *); void *user; bool cancel; } dlg_ctx_t;
static sui_window_t *g_dlg_win = NULL;

static void dlg_mask_draw(sui_widget_t *w, sui_canvas_t *c)
{
    for (int y = 0; y < w->h; y++)
        for (int x = 0; x < w->w; x++) {
            int px = w->abs_x + x, py = w->abs_y + y;
            if (px < 0 || py < 0 || px >= c->width || py >= c->height) continue;
            uint32_t *p = &c->pixels[(uint64_t)py * c->width + px];
            uint8_t r = (*p >> 16) & 0xFF, g = (*p >> 8) & 0xFF, b = *p & 0xFF;
            *p = (0xFFu << 24) | ((r * 0x60 / 0xFF) << 16) | ((g * 0x60 / 0xFF) << 8) | (b * 0x60 / 0xFF);
        }
}
static bool dlg_mask_event(sui_widget_t *w, const sui_event_t *ev)
{
    if (ev->type == SUI_EVENT_CLICK) {
        dlg_ctx_t *d = (dlg_ctx_t *)w->user_data;
        if (d && d->cancel) sui_dialog_close(g_dlg_win);
    }
    return false;
}
static const sui_widget_vtable_t g_mask_vt = { "mask", dlg_mask_draw, NULL, dlg_mask_event, NULL };

static void dlg_ok_click(sui_widget_t *w, void *u)
{
    (void)w;
    dlg_ctx_t *d = (dlg_ctx_t *)u;
    if (d && d->on_ok) d->on_ok(d->user);
    sui_dialog_close(g_dlg_win);
}
static void dlg_cancel_click(sui_widget_t *w, void *u)
{
    (void)w; (void)u;
    sui_dialog_close(g_dlg_win);
}

void sui_dialog_show(sui_window_t *win, const char *title, const char *body,
                     const char *ok_label, const char *cancel_label,
                     void (*on_ok)(void *), void *user)
{
    if (!win || !win->overlay) return;
    sui_overlay_clear(win->overlay);
    g_dlg_win = win;
    /* 遮罩 */
    sui_widget_t *mask = sui_widget_create(&g_mask_vt, win->overlay);
    mask->w = win->overlay->w; mask->h = win->overlay->h;
    dlg_ctx_t *d = (dlg_ctx_t *)sui__alloc(sizeof(dlg_ctx_t));
    d->on_ok = on_ok; d->user = user;
    d->cancel = (cancel_label && cancel_label[0]);
    mask->user_data = d;
    /* 对话框面板 */
    sui_panel_t *p = sui_panel_create(win->overlay, title);
    int pw = 380, ph = 200;
    p->base.w = pw; p->base.h = ph;
    p->base.x = (win->overlay->w - pw) / 2;
    p->base.y = (win->overlay->h - ph) / 2;
    /* 正文 */
    sui_label_t *lb = sui_label_create(&p->base, body ? body : "");
    lb->base.x = 16; lb->base.y = 54; lb->base.w = pw - 32; lb->base.h = 70;
    /* 按钮 */
    sui_button_t *ok = sui_button_create(&p->base, SUI_BTN_PRIMARY, SUI_BTN_SIZE_MD, ok_label ? ok_label : "确定");
    ok->base.x = pw - 16 - 100; ok->base.y = ph - 50; ok->base.w = 100; ok->base.h = 36;
    ok->on_click = dlg_ok_click; ok->user = d;
    if (d->cancel) {
        sui_button_t *cb = sui_button_create(&p->base, SUI_BTN_SECONDARY, SUI_BTN_SIZE_MD, cancel_label);
        cb->base.x = pw - 16 - 100 - 110; cb->base.y = ph - 50; cb->base.w = 100; cb->base.h = 36;
        cb->on_click = dlg_cancel_click; cb->user = d;
    }
    win->overlay->visible = true;
}
void sui_dialog_close(sui_window_t *win)
{
    if (!win) win = g_dlg_win;
    if (!win || !win->overlay) return;
    sui_overlay_clear(win->overlay);
}
