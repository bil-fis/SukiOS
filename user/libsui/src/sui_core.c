/*
 * user/libsui/src/sui_core.c
 * -----------------------------------------------------------------------------
 * iSuki UI 界面库核心实现：主题、画布绘制原语、控件基类、布局、事件分发、窗口。
 *
 * 依赖：libsuki_gui（窗口客户端）+ font8x8（文本字形后端）。
 * 不依赖内核；所有跨进程交互走 libsuki_gui 的 syscall/Mach IPC。
 */
#include "sui.h"
#include <string.h>

/* =========================================================================
 * 内存：简单凸增分配器（控件生命周期由父控件管理，destroy 仅解链不回收）
 * ====================================================================== */
static uint8_t *g_heap = NULL;
static size_t   g_heap_used = 0;
static size_t   g_heap_cap  = 0;

void *sui__alloc(size_t n)
{
    if (!g_heap) {
        g_heap_cap = 2 * 1024 * 1024;
        g_heap = (uint8_t *)sys_mmap(g_heap_cap, 3);
    }
    if (!g_heap) return NULL;
    n = (n + 7) & ~(size_t)7;
    if (g_heap_used + n > g_heap_cap) return NULL;
    void *p = g_heap + g_heap_used;
    g_heap_used += n;
    return p;
}

size_t sui_strlen(const char *s)
{
    size_t n = 0;
    if (s) while (s[n]) n++;
    return n;
}
#define sui__strlen sui_strlen
void sui__strcpy(char *d, const char *s)
{
    size_t i = 0;
    if (!d) return;
    if (s) for (; s[i]; i++) d[i] = s[i];
    d[i] = '\0';
}

/* =========================================================================
 * 主题
 * ====================================================================== */
static const sui_theme_t g_theme_light = {
    .name = "Light", .is_dark = false,
    .mica_base = SUI_L_MICA_BASE, .window_bg = SUI_L_WINDOW_BG, .card_bg = SUI_L_CARD_BG,
    .sidebar_bg = SUI_L_SIDEBAR_BG, .menu_bg = SUI_L_MENU_BG, .toast_bg = SUI_L_TOAST_BG,
    .stroke = SUI_L_STROKE, .stroke_soft = SUI_L_STROKE_SOFT, .divider = SUI_L_DIVIDER,
    .text_primary = SUI_L_TEXT_PRIMARY, .text_secondary = SUI_L_TEXT_SECONDARY,
    .text_tertiary = SUI_L_TEXT_TERTIARY, .text_disabled = SUI_L_TEXT_DISABLED,
    .accent = SUI_L_ACCENT, .accent_hover = SUI_L_ACCENT_HOVER,
    .accent_pressed = SUI_L_ACCENT_PRESSED, .accent_soft = SUI_L_ACCENT,
    .danger = SUI_L_DANGER, .warning = SUI_L_WARNING, .success = SUI_L_SUCCESS,
    .surface = 0x00FFFFFF, .surface_hover = 0x00F0F0F0, .surface_active = 0x00E4E4E4,
    .input_bg = 0x00FFFFFF, .switch_off = 0x00C7C7CC, .box_border = 0x00999999,
    .range_track = 0x00D0D0D0, .range_thumb = 0x00FFFFFF,
    .radius_window = SUI_RADIUS_WINDOW, .radius_control = SUI_RADIUS_CTRL,
    .titlebar_height = SUI_TITLEBAR_H, .material_opacity = 0.88f, .density = 0,
    .reduce_motion = false,
};
static const sui_theme_t g_theme_dark = {
    .name = "Dark", .is_dark = true,
    .mica_base = SUI_D_MICA_BASE, .window_bg = SUI_D_WINDOW_BG, .card_bg = SUI_D_CARD_BG,
    .sidebar_bg = SUI_D_SIDEBAR_BG, .menu_bg = SUI_D_MENU_BG, .toast_bg = SUI_D_TOAST_BG,
    .stroke = SUI_D_STROKE, .stroke_soft = SUI_D_STROKE_SOFT, .divider = SUI_D_DIVIDER,
    .text_primary = SUI_D_TEXT_PRIMARY, .text_secondary = SUI_D_TEXT_SECONDARY,
    .text_tertiary = SUI_D_TEXT_TERTIARY, .text_disabled = SUI_D_TEXT_DISABLED,
    .accent = SUI_D_ACCENT, .accent_hover = SUI_D_ACCENT_HOVER,
    .accent_pressed = SUI_D_ACCENT_PRESSED, .accent_soft = SUI_D_ACCENT,
    .danger = SUI_D_DANGER, .warning = SUI_D_WARNING, .success = SUI_D_SUCCESS,
    .surface = 0x002C2C2E, .surface_hover = 0x003A3A3C, .surface_active = 0x00464648,
    .input_bg = 0x00202022, .switch_off = 0x0045454A, .box_border = 0x006E6E73,
    .range_track = 0x0045454A, .range_thumb = 0x00FFFFFF,
    .radius_window = SUI_RADIUS_WINDOW, .radius_control = SUI_RADIUS_CTRL,
    .titlebar_height = SUI_TITLEBAR_H, .material_opacity = 0.88f, .density = 0,
    .reduce_motion = false,
};
static sui_theme_t g_theme;   /* 当前主题（可变副本） */

const sui_theme_t SUI_THEME_LIGHT = {0};
const sui_theme_t SUI_THEME_DARK  = {0};

const sui_theme_t *sui_theme(void) { return &g_theme; }
void sui_theme_set(const sui_theme_t *t) { if (t) g_theme = *t; }
void sui_theme_set_dark(bool dark) { g_theme = dark ? g_theme_dark : g_theme_light; }
bool sui_theme_is_dark(void) { return g_theme.is_dark; }
void sui_theme_set_accent(sui_accent_t preset, uint32_t custom)
{
    uint32_t c = (preset == SUI_ACCENT_CUSTOM && custom) ? custom
               : (preset != SUI_ACCENT_CUSTOM) ? (uint32_t)preset : g_theme.accent;
    g_theme.accent = c;
    /* 派生 hover/pressed/soft（简易亮度调整） */
    uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    uint32_t cl = (r * 108 / 100 > 255 ? 255 : r * 108 / 100);
    uint32_t gl = (g * 108 / 100 > 255 ? 255 : g * 108 / 100);
    uint32_t bl = (b * 108 / 100 > 255 ? 255 : b * 108 / 100);
    g_theme.accent_hover = SUI_RGB(cl, gl, bl);
    uint32_t dl = (r * 92 / 100), dgl = (g * 92 / 100), dbl = (b * 92 / 100);
    g_theme.accent_pressed = SUI_RGB(dl, dgl, dbl);
    g_theme.accent_soft = (c & 0x00FFFFFF) | 0x29000000u;
}
void sui_theme_set_radius_window(int r) { g_theme.radius_window = r; }
void sui_theme_set_radius_control(int r) { g_theme.radius_control = r; }
void sui_theme_set_reduce_motion(bool reduce) { g_theme.reduce_motion = reduce; }

static const sui_compositor_caps_t g_caps = {
    .supports_blur = false, .supports_alpha = true,
    .supports_wallpaper_sample = false, .supports_rounded_corner = true,
    .supports_shadow = true,
};
const sui_compositor_caps_t *sui_compositor_caps(void) { return &g_caps; }

/* =========================================================================
 * 画布绘制原语
 * ====================================================================== */
sui_canvas_t *sui_canvas_create(sui_window_t *win)
{
    if (!win || !win->wk) return NULL;
    sui_canvas_t *c = (sui_canvas_t *)sui__alloc(sizeof(sui_canvas_t));
    if (!c) return NULL;
    c->win = win;
    c->pixels = (uint32_t *)win->wk->buffer;
    c->width = (int)win->wk->w;
    c->height = (int)win->wk->h;
    c->clip_x = 0; c->clip_y = 0; c->clip_w = c->width; c->clip_h = c->height;
    c->alpha = 1.0f;
    return c;
}
void sui_canvas_destroy(sui_canvas_t *c) { (void)c; }  /* 凸增分配，不回收 */

void sui_canvas_flush(sui_canvas_t *c, int x, int y, int w, int h)
{
    if (!c || !c->win) return;
    sui_window_present(c->win, x, y, w, h);
}

static inline bool sui__in_clip(sui_canvas_t *c, int x, int y)
{
    return x >= c->clip_x && y >= c->clip_y &&
           x < c->clip_x + c->clip_w && y < c->clip_y + c->clip_h;
}

void sui_canvas_set_pixel(sui_canvas_t *c, int x, int y, uint32_t color)
{
    if (!c) return;
    if (!sui__in_clip(c, x, y)) return;
    if ((uint32_t)x >= (uint32_t)c->width || (uint32_t)y >= (uint32_t)c->height) return;
    c->pixels[(uint64_t)y * c->width + x] = SUI_FB(color);
}

void sui_canvas_fill_rect(sui_canvas_t *c, int x, int y, int w, int h, uint32_t color)
{
    if (!c) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w, y1 = y + h;
    if (x1 > c->width) x1 = c->width;
    if (y1 > c->height) y1 = c->height;
    if (x0 < c->clip_x) x0 = c->clip_x;
    if (y0 < c->clip_y) y0 = c->clip_y;
    if (x1 > c->clip_x + c->clip_w) x1 = c->clip_x + c->clip_w;
    if (y1 > c->clip_y + c->clip_h) y1 = c->clip_y + c->clip_h;
    if (x1 <= x0 || y1 <= y0) return;
    uint32_t col = SUI_FB(color);
    for (int j = y0; j < y1; j++) {
        uint32_t *row = c->pixels + (uint64_t)j * c->width + x0;
        for (int i = x0; i < x1; i++) row[i] = col;
    }
}

void sui_canvas_draw_line(sui_canvas_t *c, int x0, int y0, int x1, int y1,
                          uint32_t color, int thickness)
{
    if (!c) return;
    int dx = x1 - x0, dy = y1 - y0;
    int steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx)
                                                         : (dy < 0 ? -dy : dy);
    if (steps == 0) { sui_canvas_fill_rect(c, x0, y0, thickness, thickness, color); return; }
    int t = thickness < 1 ? 1 : thickness;
    for (int s = 0; s <= steps; s++) {
        int x = x0 + (int)((int64_t)dx * s / steps);
        int y = y0 + (int)((int64_t)dy * s / steps);
        sui_canvas_fill_rect(c, x - t / 2, y - t / 2, t, t, color);
    }
}

/* 圆角四角中点圆填充（quad: 0=右下? 这里 0=左上,1=右上,2=左下,3=右下） */
void sui__fill_corner(sui_canvas_t *c, int cx, int cy, int r, uint32_t color, int quad)
{
    if (r <= 0) return;
    int rr = r * r;
    for (int y = 0; y < r; y++) {
        for (int x = 0; x < r; x++) {
            if (x * x + y * y > rr) continue;
            int px, py;
            switch (quad) {
                case 0: px = cx - r + x; py = cy - r + y; break;
                case 1: px = cx + x;     py = cy - r + y; break;
                case 2: px = cx - r + x; py = cy + y;     break;
                default:px = cx + x;     py = cy + y;     break;
            }
            sui_canvas_set_pixel(c, px, py, color);
        }
    }
}

void sui_canvas_fill_rounded_rect(sui_canvas_t *c, int x, int y, int w, int h,
                                  int r, uint32_t color)
{
    if (!c) return;
    if (r <= 0) { sui_canvas_fill_rect(c, x, y, w, h, color); return; }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    sui_canvas_fill_rect(c, x, y + r, w, h - 2 * r, color);
    sui_canvas_fill_rect(c, x + r, y, w - 2 * r, r, color);
    sui_canvas_fill_rect(c, x + r, y + h - r, w - 2 * r, r, color);
    sui__fill_corner(c, x + r,     y + r,     r, color, 0);
    sui__fill_corner(c, x + w - r, y + r,     r, color, 1);
    sui__fill_corner(c, x + r,     y + h - r, r, color, 2);
    sui__fill_corner(c, x + w - r, y + h - r, r, color, 3);
}

void sui_canvas_stroke_rounded_rect(sui_canvas_t *c, int x, int y, int w, int h,
                                    int r, int thickness, uint32_t color)
{
    if (!c) return;
    int t = thickness < 1 ? 1 : thickness;
    /* 简化：外填充圆角矩形 - 内填充背景近似描边（自包含，不依赖背景读取） */
    sui_canvas_fill_rounded_rect(c, x, y, w, h, r, color);
    sui_canvas_fill_rounded_rect(c, x + t, y + t, w - 2 * t, h - 2 * t,
                                 r > t ? r - t : 0, 0x00000000u);
}

void sui_canvas_fill_circle(sui_canvas_t *c, int cx, int cy, int r, uint32_t color)
{
    if (!c || r <= 0) return;
    int rr = r * r;
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= rr) sui_canvas_set_pixel(c, cx + x, cy + y, color);
}
void sui_canvas_stroke_circle(sui_canvas_t *c, int cx, int cy, int r, int thickness, uint32_t color)
{
    if (!c || r <= 0) return;
    int t = thickness < 1 ? 1 : thickness;
    int ro = r + t, ri = r - t;
    int ro2 = ro * ro, ri2 = ri * ri;
    for (int y = -ro; y <= ro; y++)
        for (int x = -ro; x <= ro; x++) {
            int d = x * x + y * y;
            if (d <= ro2 && d >= ri2) sui_canvas_set_pixel(c, cx + x, cy + y, color);
        }
}

void sui_canvas_draw_shadow(sui_canvas_t *c, int x, int y, int w, int h,
                            int radius, int blur, uint32_t color)
{
    if (!c) return;
    (void)blur;
    /* 简化阴影：在矩形四周外扩 2px 画半透明描边近似（不读背景，直接叠加暗色） */
    int pad = 2;
    uint32_t sh = (color & 0x00FFFFFF) | 0x20000000u;
    sui_canvas_fill_rounded_rect(c, x - pad, y - pad, w + 2 * pad, h + 2 * pad,
                                 radius + pad, sh);
}

/* 文本：8x8 点阵字形，按字号缩放（scale = max(1, font_size/12)） */
static void sui__draw_glyph(sui_canvas_t *c, int gx, int gy, int scale, uint32_t color,
                            const uint8_t *g)
{
    for (int row = 0; row < 8; row++) {
        uint8_t bits = g[row];
        if (!bits) continue;
        for (int col = 0; col < 8; col++) {
            if (bits & (1u << col)) {
                if (scale <= 1)
                    sui_canvas_set_pixel(c, gx + col, gy + row, color);
                else
                    sui_canvas_fill_rect(c, gx + col * scale, gy + row * scale,
                                         scale, scale, color);
            }
        }
    }
}

void sui_canvas_draw_text(sui_canvas_t *c, int x, int y, const char *text,
                          int font_size, int weight, uint32_t color)
{
    if (!c || !text) return;
    (void)weight;
    int scale = font_size <= 0 ? 1 : (font_size + 5) / 12;
    if (scale < 1) scale = 1;
    int pen_x = x;
    for (const char *p = text; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch < 0x20 || ch > 0x7E) ch = '?';
        sui__draw_glyph(c, pen_x, y, scale, color, font8x8_basic[ch]);
        pen_x += 8 * scale;
    }
}
int sui_canvas_measure_text(sui_canvas_t *c, const char *text, int font_size, int weight)
{
    (void)c; (void)weight;
    int scale = font_size <= 0 ? 1 : (font_size + 5) / 12;
    if (scale < 1) scale = 1;
    return (int)sui__strlen(text) * 8 * scale;
}

void sui_canvas_push_clip(sui_canvas_t *c, int x, int y, int w, int h)
{
    if (!c) return;
    int nx0 = x < c->clip_x ? c->clip_x : x;
    int ny0 = y < c->clip_y ? c->clip_y : y;
    int nx1 = (x + w > c->clip_x + c->clip_w) ? c->clip_x + c->clip_w : x + w;
    int ny1 = (y + h > c->clip_y + c->clip_h) ? c->clip_y + c->clip_h : y + h;
    c->clip_x = nx0; c->clip_y = ny0; c->clip_w = nx1 - nx0; c->clip_h = ny1 - ny0;
}
void sui_canvas_pop_clip(sui_canvas_t *c)
{
    if (!c) return;
    /* 单级：恢复为整窗（本实现不维护栈，调用方需对称） */
    c->clip_x = 0; c->clip_y = 0; c->clip_w = c->width; c->clip_h = c->height;
}

void sui_canvas_draw_background(sui_canvas_t *c, const sui_widget_t *w, int radius,
                                uint32_t bg, int shadow_blur, uint32_t shadow_color)
{
    if (!c || !w) return;
    if (shadow_blur > 0) sui_canvas_draw_shadow(c, w->abs_x, w->abs_y, w->w, w->h,
                                               radius, shadow_blur, shadow_color);
    uint32_t col = bg ? bg : sui_theme()->surface;
    sui_canvas_fill_rounded_rect(c, w->abs_x, w->abs_y, w->w, w->h, radius, col);
}

void sui_canvas_draw_focus_ring(sui_canvas_t *c, const sui_widget_t *w)
{
    if (!c || !w) return;
    const sui_theme_t *t = sui_theme();
    sui_canvas_stroke_rounded_rect(c, w->abs_x - 2, w->abs_y - 2, w->w + 4, w->h + 4,
                                   w->corner_radius + 2, 2, t->accent);
}

/* =========================================================================
 * 控件基类
 * ====================================================================== */
sui_widget_t *sui_widget_create(const sui_widget_vtable_t *vt, sui_widget_t *parent)
{
    sui_widget_t *w = (sui_widget_t *)sui__alloc(sizeof(sui_widget_t));
    if (!w) return NULL;
    memset(w, 0, sizeof(*w));
    w->vtable = vt;
    w->visible = true; w->enabled = true; w->focusable = true; w->opacity = 1.0f;
    w->corner_radius = SUI_RADIUS_CTRL;
    w->layout.type = SUI_LAYOUT_ABSOLUTE;
    w->constraint.min_w = w->constraint.min_h = 0;
    w->constraint.max_w = w->constraint.max_h = 100000;
    w->constraint.preferred_w = w->constraint.preferred_h = -1;
    w->constraint.flex = 0;
    if (parent) sui_widget_add_child(parent, w);
    return w;
}
void sui_widget_destroy(sui_widget_t *w)
{
    if (!w) return;
    while (w->first_child) sui_widget_destroy(w->first_child);
    if (w->parent) sui_widget_remove_child(w->parent, w);
    if (w->vtable && w->vtable->destroy) w->vtable->destroy(w);
}
void sui_widget_destroy_recursive(sui_widget_t *w) { sui_widget_destroy(w); }

void sui_widget_add_child(sui_widget_t *parent, sui_widget_t *child)
{
    if (!parent || !child) return;
    if (child->parent) sui_widget_remove_child(child->parent, child);
    child->parent = parent;
    child->next_sibling = NULL;
    child->prev_sibling = parent->last_child;
    if (parent->last_child) parent->last_child->next_sibling = child;
    else parent->first_child = child;
    parent->last_child = child;
    parent->child_count++;
}
void sui_widget_remove_child(sui_widget_t *parent, sui_widget_t *child)
{
    if (!parent || !child || child->parent != parent) return;
    if (child->prev_sibling) child->prev_sibling->next_sibling = child->next_sibling;
    else parent->first_child = child->next_sibling;
    if (child->next_sibling) child->next_sibling->prev_sibling = child->prev_sibling;
    else parent->last_child = child->prev_sibling;
    parent->child_count--;
    child->parent = NULL; child->prev_sibling = child->next_sibling = NULL;
}
void sui_widget_set_parent(sui_widget_t *w, sui_widget_t *new_parent)
{
    sui_widget_add_child(new_parent, w);
}
void sui_widget_set_rect(sui_widget_t *w, int x, int y, int w_, int h_)
{
    if (!w) return;
    w->x = x; w->y = y; w->w = w_; w->h = h_; w->dirty = true;
}
void sui_widget_set_pos(sui_widget_t *w, int x, int y) { if (w) { w->x = x; w->y = y; w->dirty = true; } }
void sui_widget_set_size(sui_widget_t *w, int w_, int h_) { if (w) { w->w = w_; w->h = h_; w->dirty = true; } }
void sui_widget_set_visible(sui_widget_t *w, bool v) { if (w) { w->visible = v; w->dirty = true; } }
void sui_widget_set_enabled(sui_widget_t *w, bool e) { if (w) { w->enabled = e; w->dirty = true; } }
void sui_widget_set_text(sui_widget_t *w, const char *text) { if (w) sui__strcpy(w->text, text); }
void sui_widget_set_layout(sui_widget_t *w, const sui_layout_params_t *params)
{
    if (w && params) { w->layout = *params; w->dirty = true; }
}
void sui_widget_set_constraint(sui_widget_t *w, const sui_size_constraint_t *c)
{
    if (w && c) { w->constraint = *c; w->dirty = true; }
}

static sui_widget_t *g_focus = NULL;
void sui_widget_focus(sui_widget_t *w)
{
    if (!w || !w->focusable) return;
    if (g_focus && g_focus != w) g_focus->focused = false;
    g_focus = w; w->focused = true;
}
void sui_widget_blur(sui_widget_t *w)
{
    if (w && g_focus == w) { g_focus = NULL; w->focused = false; }
}
sui_widget_t *sui_widget_focused(void) { return g_focus; }

void sui_widget_invalidate(sui_widget_t *w) { if (w) w->dirty = true; }
void sui_widget_invalidate_rect(sui_widget_t *w, int x, int y, int w_, int h_)
{
    (void)x; (void)y; (void)w_; (void)h_;
    if (w) w->dirty = true;
}
void sui_widget_invalidate_layout(sui_widget_t *w) { if (w) w->dirty = true; }

sui_widget_t *sui_widget_hit_test(sui_widget_t *root, int x, int y)
{
    if (!root || !root->visible) return NULL;
    for (sui_widget_t *c = root->last_child; c; c = c->prev_sibling) {
        sui_widget_t *hit = sui_widget_hit_test(c, x, y);
        if (hit) return hit;
    }
    int ax = root->abs_x, ay = root->abs_y;
    if (x >= ax && x < ax + root->w && y >= ay && y < ay + root->h) return root;
    return NULL;
}

void sui_widget_to_screen(sui_widget_t *w, int *x, int *y)
{
    int ax = 0, ay = 0;
    for (sui_widget_t *p = w; p; p = p->parent) { ax += p->x; ay += p->y; }
    if (x) *x = ax;
    if (y) *y = ay;
}
void sui_widget_from_screen(sui_widget_t *w, int *x, int *y)
{
    int ax = 0, ay = 0;
    for (sui_widget_t *p = w; p; p = p->parent) { ax += p->x; ay += p->y; }
    if (x) *x = *x - ax;
    if (y) *y = *y - ay;
}

/* 布局：绝对 + 流式（FLOW 按主轴排列子项，交叉轴居中） */
void sui_layout_apply(sui_widget_t *root)
{
    if (!root) return;
    int ax = root->abs_x, ay = root->abs_y;
    root->abs_x = ax; root->abs_y = ay;
    if (root->layout.type == SUI_LAYOUT_FLOW) {
        sui_layout_params_t *L = &root->layout;
        int pad_t = L->padding[0], pad_l = L->padding[3];
        int cx = root->x + pad_l, cy = root->y + pad_t;
        int max_cross = 0;
        for (sui_widget_t *c = root->first_child; c; c = c->next_sibling) {
            if (!c->visible) continue;
            if (L->axis == SUI_AXIS_HORIZONTAL) {
                c->abs_x = root->abs_x + cx;
                c->abs_y = root->abs_y + cy;
                cx += c->w + L->gap;
                if (c->h > max_cross) max_cross = c->h;
            } else {
                c->abs_x = root->abs_x + cx;
                c->abs_y = root->abs_y + cy;
                cy += c->h + L->gap;
                if (c->w > max_cross) max_cross = c->w;
            }
        }
        (void)max_cross;
    } else {
        for (sui_widget_t *c = root->first_child; c; c = c->next_sibling) {
            c->abs_x = root->abs_x + c->x;
            c->abs_y = root->abs_y + c->y;
        }
    }
    for (sui_widget_t *c = root->first_child; c; c = c->next_sibling)
        sui_layout_apply(c);
}

/* =========================================================================
 * 渲染（规范 7.4）
 * ====================================================================== */
void sui_render(sui_widget_t *root, sui_canvas_t *canvas)
{
    if (!root || !canvas || !root->visible) return;
    const sui_theme_t *t = sui_theme();
    if (root->bg_color || root->material != SUI_MATERIAL_SOLID) {
        sui_canvas_draw_background(canvas, root, root->corner_radius,
                                   root->bg_color, 0, 0);
    } else if (root->vtable == NULL && root->parent == NULL) {
        /* 根：不强制背景（窗口背景已由 render_window 填充） */
    }
    if (root->vtable && root->vtable->draw)
        root->vtable->draw(root, canvas);
    if (root->focused && root->focusable)
        sui_canvas_draw_focus_ring(canvas, root);
    for (sui_widget_t *c = root->first_child; c; c = c->next_sibling)
        sui_render(c, canvas);
    (void)t;
}

/* =========================================================================
 * 事件分发（规范 7.5 / 13.2）
 * ====================================================================== */
void sui_event_stop(sui_event_t *ev) { if (ev) ev->stopped = true; }

static sui_widget_t *g_hover = NULL;

void sui_dispatch_event(sui_widget_t *root, const sui_event_t *ev)
{
    if (!root) return;
    if (ev->type >= SUI_EVENT_MOUSE_MOVE && ev->type <= SUI_EVENT_MOUSE_WHEEL) {
        sui_widget_t *target = sui_widget_hit_test(root, ev->x, ev->y);
        if (target) {
            if (g_hover && g_hover != target) g_hover->hovered = false;
            g_hover = target; target->hovered = true;
            if (ev->type == SUI_EVENT_MOUSE_DOWN && (ev->button & 1)) target->pressed = true;
            for (sui_widget_t *w = target; w; w = w->parent) {
                if (w->vtable && w->vtable->on_event) w->vtable->on_event(w, ev);
                if (ev->stopped) break;
            }
            if (ev->type == SUI_EVENT_MOUSE_UP && (ev->button & 1)) {
                if (target->pressed) {
                    target->pressed = false;
                    sui_event_t click = *ev;
                    click.type = SUI_EVENT_CLICK;
                    for (sui_widget_t *w = target; w; w = w->parent) {
                        if (w->vtable && w->vtable->on_event) w->vtable->on_event(w, &click);
                        if (click.stopped) break;
                    }
                }
                target->pressed = false;
            }
        }
    } else if (ev->type == SUI_EVENT_KEY_DOWN || ev->type == SUI_EVENT_KEY_UP ||
               ev->type == SUI_EVENT_KEY_CHAR) {
        sui_widget_t *f = sui_widget_focused();
        if (f) {
            for (sui_widget_t *w = f; w; w = w->parent) {
                if (w->vtable && w->vtable->on_event) w->vtable->on_event(w, ev);
                if (ev->stopped) break;
            }
        }
    } else {
        for (sui_widget_t *w = root; w; w = w->parent) {
            if (w->vtable && w->vtable->on_event) w->vtable->on_event(w, ev);
            if (ev->stopped) break;
        }
    }
}

/* =========================================================================
 * 窗口
 * ====================================================================== */
int sui_init(sui_window_t *win)
{
    (void)win;
    g_theme = g_theme_light;   /* 默认浅色主题 */
    return 0;
}

void sui_shutdown(void) { g_focus = NULL; g_hover = NULL; }

sui_window_t *sui_create_window(const char *title, int x, int y, int w, int h, bool titlebar)
{
    sui_window_t *win = (sui_window_t *)sui__alloc(sizeof(sui_window_t));
    if (!win) return NULL;
    memset(win, 0, sizeof(*win));
    uint32_t style = SUKI_WS_DEFAULT;
    if (!titlebar) style &= ~SUKI_WS_TITLEBAR;
    win->wk = SukiCreateWindow(title ? title : "SukiOS", x, y, (uint32_t)w, (uint32_t)h, style);
    if (!win->wk) { return NULL; }
    win->has_titlebar = titlebar;
    win->bg = SUI_FB(g_theme.window_bg);
    sui__strcpy(win->title, title ? title : "");
    win->running = true;
    win->root = sui_widget_create(NULL, NULL);
    if (!win->root) { SukiDestroyWindow(win->wk); return NULL; }
    win->root->w = w; win->root->h = h;
    win->root->focusable = false;
    return win;
}

void sui_destroy_window(sui_window_t *win)
{
    if (!win) return;
    if (win->root) sui_widget_destroy(win->root);
    if (win->wk) SukiDestroyWindow(win->wk);
    win->running = false;
}

void sui_window_set_bg(sui_window_t *win, uint32_t rgb) { if (win) win->bg = SUI_FB(rgb); }
void sui_window_set_title(sui_window_t *win, const char *title)
{
    if (win) sui__strcpy(win->title, title);
}
void sui_window_set_event_port(sui_window_t *win)
{
    if (!win || !win->wk) return;
    uint32_t ep = sys_port_alloc();
    if (ep) { sys_port_claim(ep); SukiSetEventPort(win->wk, ep); }
}

void sui_render_window(sui_window_t *win)
{
    if (!win || !win->wk) return;
    sui_canvas_t *c = sui_canvas_create(win);
    if (!c) return;
    const sui_theme_t *t = sui_theme();
    /* 窗口背景（纯色，取自主题） */
    sui_canvas_fill_rect(c, 0, 0, c->width, c->height, win->bg);
    int client_y = 0;
    if (win->has_titlebar) {
        int th = t->titlebar_height;
        sui_canvas_fill_rect(c, 0, 0, c->width, th, t->accent);
        /* 交通灯（左起 3 个圆点） */
        int cy = th / 2;
        sui_canvas_fill_circle(c, 16, cy, 6, SUI_TRAFFIC_CLOSE);
        sui_canvas_fill_circle(c, 34, cy, 6, SUI_TRAFFIC_MIN);
        sui_canvas_fill_circle(c, 52, cy, 6, SUI_TRAFFIC_MAX);
        /* 标题（居中） */
        int tw = sui_canvas_measure_text(c, win->title, SUI_FONT_SUBTITLE, SUI_WEIGHT_MEDIUM);
        int tx = (c->width - tw) / 2;
        sui_canvas_draw_text(c, tx, cy - 6, win->title, SUI_FONT_SUBTITLE,
                             SUI_WEIGHT_MEDIUM, 0x00FFFFFFu);
        client_y = th;
    }
    /* 客户区裁剪后渲染根控件 */
    sui_canvas_push_clip(c, 0, client_y, c->width, c->height - client_y);
    sui_layout_apply(win->root);
    sui_render(win->root, c);
    sui_canvas_pop_clip(c);
    sui_canvas_destroy(c);
}

void sui_window_present(sui_window_t *win, int x, int y, int w, int h)
{
    if (!win || !win->wk) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if (x + w > (int)win->wk->w) w = (int)win->wk->w - x;
    if (y + h > (int)win->wk->h) h = (int)win->wk->h - y;
    if (w <= 0 || h <= 0) return;
    SukiFlush(win->wk, x, y, (uint32_t)w, (uint32_t)h);
}

bool sui_window_poll_event(sui_window_t *win, sui_event_t *out)
{
    if (!win || !win->wk || !out) return false;
    suki_event_t ke;
    if (!SukiPollEvent(win->wk, &ke)) return false;
    memset(out, 0, sizeof(*out));
    out->timestamp = ke.timestamp;
    switch (ke.type) {
        case SUKI_EVENT_MOUSE_MOVE: out->type = SUI_EVENT_MOUSE_MOVE; break;
        case SUKI_EVENT_MOUSE_DOWN:  out->type = SUI_EVENT_MOUSE_DOWN;  break;
        case SUKI_EVENT_MOUSE_UP:    out->type = SUI_EVENT_MOUSE_UP;    break;
        case SUKI_EVENT_MOUSE_WHEEL: out->type = SUI_EVENT_MOUSE_WHEEL; break;
        case SUKI_EVENT_KEY_DOWN:    out->type = SUI_EVENT_KEY_DOWN;    break;
        case SUKI_EVENT_KEY_UP:      out->type = SUI_EVENT_KEY_UP;      break;
        case SUKI_EVENT_WINDOW_CLOSE:out->type = SUI_EVENT_WINDOW_CLOSE;break;
        default: out->type = SUI_EVENT_NONE; break;
    }
    out->x = (int)ke.u.mouse.x; out->y = (int)ke.u.mouse.y;
    out->button = ke.u.mouse.buttons;
    out->key = ke.u.key.keycode; out->mods = ke.u.key.modifiers;
    return true;
}

/* 单帧：拉取并处理一个事件（命中/焦点分发），返回是否处理了事件 */
bool sui_window_step(sui_window_t *win, sui_event_t *out)
{
    sui_event_t ev;
    if (!sui_window_poll_event(win, &ev)) return false;
    if (out) *out = ev;
    if (ev.type == SUI_EVENT_WINDOW_CLOSE) { win->running = false; return true; }
    sui_dispatch_event(win->root, &ev);
    sui_render_window(win);
    sui_window_present(win, 0, 0, win->wk->w, win->wk->h);
    return true;
}
