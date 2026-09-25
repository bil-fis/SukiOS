/*
 * user/libsui/include/sui.h
 * -----------------------------------------------------------------------------
 * iSuki UI 界面库（libsui）公共入口。
 *
 * 依据《iSuki UI 界面库设计规范 v1.0》实现，作为 SukiOS 用户态原生控件库。
 * 设计约束（来自规范）：
 *   - 不依赖内核，只依赖 libsuki_gui（窗口客户端）+ libsuki 字体原语；
 *   - 所有渲染发生在应用离屏缓冲，display_server 只做合成；
 *   - 单条 OOL 消息上限 16 页，大窗口分片提交（由 libsuki_gui 负责）；
 *   - 所有颜色来自主题结构体，禁止硬编码（交通灯/语义色除外）；
 *   - 控件必须支持键盘焦点与禁用状态。
 *
 * 像素格式：xRGB32（0x00RRGGBB）。主题颜色以 0xAARRGGBB 存储，绘制时取低 24 位。
 *
 * 说明：本文件为单一总入口（规范 15.1 的各子头经此聚合），便于「每个源文件独立编译」。
 */
#ifndef SUI_H
#define SUI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "lib/suki_gui.h"   /* suki_window_t / Suki* 窗口客户端 */
#include "lib/font8x8.h"    /* 8x8 点阵字形（文本渲染后端，规范字体服务的等价降级） */

/* 不依赖 libc 的字符串长度（freestanding 环境） */
size_t sui_strlen(const char *s);
/* 内部凸增分配器（控件内存；destroy 仅解链不回收） */
void *sui__alloc(size_t n);
void sui__strcpy(char *d, const char *s);

/* 前向声明：控件基类 vtable 引用到画布 / 事件类型 */
typedef struct sui_canvas sui_canvas_t;
typedef struct sui_event sui_event_t;

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * 1. 颜色辅助
 * ====================================================================== */
typedef uint32_t sui_color_t;   /* 0xAARRGGBB */
#define SUI_RGB(r, g, b)       (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define SUI_ARGB(a, r, g, b)   (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | \
                                ((uint32_t)(g) << 8) | (uint32_t)(b))
/* 帧缓冲写入：丢弃 alpha（窗口离屏缓冲为不透明 xRGB32） */
#define SUI_FB(c)             ((c) & 0x00FFFFFFu)

/* =========================================================================
 * 2. 命名前缀（规范 2.1）
 *   库总入口 sui_；控件 sui_<控件>_；内部 sui__；宏 SUI_；枚举 sui_<类型>_t。
 * ====================================================================== */

/* =========================================================================
 * 3. 设计令牌（颜色 / 间距 / 圆角 / 字号，规范 3）
 * ====================================================================== */
/* 浅色主题颜色令牌 */
#define SUI_L_MICA_BASE    0xFFF3F3F3
#define SUI_L_WINDOW_BG    0xCCFFFFFF
#define SUI_L_CARD_BG      0xB3FFFFFF
#define SUI_L_SIDEBAR_BG   0x8CFFFFFF
#define SUI_L_MENU_BG      0xD9FCFCFE
#define SUI_L_TOAST_BG     0xF0FCFCFE
#define SUI_L_STROKE       0x1A000000
#define SUI_L_STROKE_SOFT  0x0F000000
#define SUI_L_DIVIDER      0x14000000
#define SUI_L_TEXT_PRIMARY 0xFF1D1D1F
#define SUI_L_TEXT_SECONDARY 0xFF6E6E73
#define SUI_L_TEXT_TERTIARY  0xFFA1A1A6
#define SUI_L_TEXT_DISABLED  0x611D1D1F
#define SUI_L_ACCENT       0xFF0A84FF
#define SUI_L_ACCENT_HOVER 0xFF2B95FF
#define SUI_L_ACCENT_PRESSED 0xFF0060DF
#define SUI_L_DANGER       0xFFFF453A
#define SUI_L_WARNING      0xFFFF9F0A
#define SUI_L_SUCCESS      0xFF30D158
#define SUI_TRAFFIC_CLOSE  0xFFFF5F57
#define SUI_TRAFFIC_MIN    0xFFFEBC2E
#define SUI_TRAFFIC_MAX    0xFF28C840

/* 深色主题颜色令牌 */
#define SUI_D_MICA_BASE    0xFF1C1C1E
#define SUI_D_WINDOW_BG    0xE01C1C1E
#define SUI_D_CARD_BG      0x0BFFFFFF
#define SUI_D_SIDEBAR_BG   0x08FFFFFF
#define SUI_D_MENU_BG      0xDB2E2E2E
#define SUI_D_TOAST_BG     0xEB303030
#define SUI_D_STROKE       0x1AFFFFFF
#define SUI_D_STROKE_SOFT  0x0FFFFFFF
#define SUI_D_DIVIDER      0x14FFFFFF
#define SUI_D_TEXT_PRIMARY 0xFFF5F5F7
#define SUI_D_TEXT_SECONDARY 0xFF98989D
#define SUI_D_TEXT_TERTIARY  0xFF6E6E73
#define SUI_D_TEXT_DISABLED  0x61F5F5F7
#define SUI_D_ACCENT       0xFF0A84FF
#define SUI_D_ACCENT_HOVER 0xFF2B95FF
#define SUI_D_ACCENT_PRESSED 0xFF0060DF
#define SUI_D_DANGER       0xFFFF453A
#define SUI_D_WARNING      0xFFFF9F0A
#define SUI_D_SUCCESS      0xFF30D158

/* 间距令牌（基数 4px） */
#define SUI_SPACE_0  0
#define SUI_SPACE_1  4
#define SUI_SPACE_2  8
#define SUI_SPACE_3  12
#define SUI_SPACE_4  16
#define SUI_SPACE_5  20
#define SUI_SPACE_6  24
#define SUI_SPACE_8  32
#define SUI_SPACE_10 40
#define SUI_SPACE_12 48
#define SUI_SPACE_16 64

/* 圆角令牌 */
#define SUI_RADIUS_NONE   0
#define SUI_RADIUS_SM     4
#define SUI_RADIUS_CTRL   6
#define SUI_RADIUS_MD     8
#define SUI_RADIUS_LG     10
#define SUI_RADIUS_WINDOW 12
#define SUI_RADIUS_FULL   9999

/* 字号阶梯 */
typedef enum {
    SUI_FONT_CAPTION = 11,
    SUI_FONT_FOOTNOTE = 12,
    SUI_FONT_BODY = 13,
    SUI_FONT_SUBTITLE = 15,
    SUI_FONT_TITLE = 20,
    SUI_FONT_LARGE = 24,
    SUI_FONT_DISPLAY = 28
} sui_font_size_t;

typedef enum {
    SUI_WEIGHT_REGULAR = 400,
    SUI_WEIGHT_MEDIUM = 500,
    SUI_WEIGHT_SEMIBOLD = 600,
    SUI_WEIGHT_BOLD = 700
} sui_font_weight_t;

/* 强调色预设 */
typedef enum {
    SUI_ACCENT_BLUE = 0xFF0A84FF,
    SUI_ACCENT_PURPLE = 0xFF7B5CFF,
    SUI_ACCENT_PINK = 0xFFFF375F,
    SUI_ACCENT_RED = 0xFFFF453A,
    SUI_ACCENT_ORANGE = 0xFFFF9F0A,
    SUI_ACCENT_GREEN = 0xFF30D158,
    SUI_ACCENT_CYAN = 0xFF64D2FF,
    SUI_ACCENT_YELLOW = 0xFFFFD60A,
    SUI_ACCENT_CUSTOM = 0x00000000
} sui_accent_t;

/* =========================================================================
 * 4. 主题系统（规范 4）
 * ====================================================================== */
typedef struct {
    const char *name;
    bool        is_dark;
    uint32_t mica_base, window_bg, card_bg, sidebar_bg, menu_bg, toast_bg;
    uint32_t stroke, stroke_soft, divider;
    uint32_t text_primary, text_secondary, text_tertiary, text_disabled;
    uint32_t accent, accent_hover, accent_pressed, accent_soft;
    uint32_t danger, warning, success;
    uint32_t surface, surface_hover, surface_active, input_bg;
    uint32_t switch_off, box_border, range_track, range_thumb;
    int  radius_window, radius_control;
    int  titlebar_height;
    float material_opacity;
    int  density;
    bool reduce_motion;
} sui_theme_t;

extern const sui_theme_t SUI_THEME_LIGHT;
extern const sui_theme_t SUI_THEME_DARK;

const sui_theme_t *sui_theme(void);
void sui_theme_set(const sui_theme_t *theme);
void sui_theme_set_dark(bool dark);
bool sui_theme_is_dark(void);
void sui_theme_set_accent(sui_accent_t preset, uint32_t custom);
void sui_theme_set_radius_window(int r);
void sui_theme_set_radius_control(int r);
void sui_theme_set_reduce_motion(bool reduce);

/* =========================================================================
 * 5. 材质系统（规范 5，精简：记录类型/透明度，合成降级由合成器负责）
 * ====================================================================== */
typedef enum {
    SUI_MATERIAL_SOLID = 0,
    SUI_MATERIAL_SMOKE,
    SUI_MATERIAL_MICA,
    SUI_MATERIAL_ACRYLIC,
    SUI_MATERIAL_VIBRANT
} sui_material_t;

typedef struct {
    sui_material_t type;
    float          opacity;
    int            blur_radius;
    int            noise_amount;
    bool           sample_wallpaper;
    uint32_t       tint;
} sui_material_desc_t;

/* 复合器能力（本实现假定支持基本圆角/阴影/alpha） */
typedef struct {
    bool supports_blur;
    bool supports_alpha;
    bool supports_wallpaper_sample;
    bool supports_rounded_corner;
    bool supports_shadow;
} sui_compositor_caps_t;

const sui_compositor_caps_t *sui_compositor_caps(void);

/* =========================================================================
 * 6. 布局（规范 6）
 * ====================================================================== */
typedef enum {
    SUI_LAYOUT_ABSOLUTE = 0,
    SUI_LAYOUT_FLOW,
    SUI_LAYOUT_GRID,
    SUI_LAYOUT_DOCK
} sui_layout_t;

typedef enum { SUI_AXIS_HORIZONTAL = 0, SUI_AXIS_VERTICAL } sui_axis_t;
typedef enum { SUI_ALIGN_START = 0, SUI_ALIGN_CENTER, SUI_ALIGN_END, SUI_ALIGN_STRETCH } sui_align_t;
typedef enum { SUI_JUSTIFY_START = 0, SUI_JUSTIFY_CENTER, SUI_JUSTIFY_END,
               SUI_JUSTIFY_SPACE_BETWEEN, SUI_JUSTIFY_SPACE_AROUND } sui_justify_t;

typedef struct {
    sui_layout_t  type;
    sui_axis_t    axis;
    int           gap;
    int           padding[4];   /* 上 右 下 左 */
    sui_align_t   align;
    sui_justify_t justify;
    int           columns;
    int           row_gap;
} sui_layout_params_t;

typedef struct {
    int min_w, max_w, min_h, max_h;
    int preferred_w, preferred_h;
    int flex;
} sui_size_constraint_t;

#define SUI_LAYOUT_ROW(g) (sui_layout_params_t){ .type=SUI_LAYOUT_FLOW, .axis=SUI_AXIS_HORIZONTAL, .gap=(g), .align=SUI_ALIGN_CENTER }
#define SUI_LAYOUT_COL(g) (sui_layout_params_t){ .type=SUI_LAYOUT_FLOW, .axis=SUI_AXIS_VERTICAL, .gap=(g) }

/* =========================================================================
 * 7. 控件基类（规范 7）
 * ====================================================================== */
struct sui_widget;
typedef struct sui_widget sui_widget_t;

struct sui_widget {
    sui_widget_t *parent;
    sui_widget_t *first_child, *last_child;
    sui_widget_t *prev_sibling, *next_sibling;
    int child_count;

    int x, y, w, h;          /* 相对父控件 */
    int abs_x, abs_y;        /* 布局后绝对坐标 */
    sui_size_constraint_t constraint;

    bool visible, enabled, focusable, focused, hovered, pressed, dirty;

    sui_layout_params_t layout;
    sui_material_t      material;
    float               opacity;
    int                 corner_radius;
    uint32_t            bg_color;   /* 0 = 使用主题默认 */

    void (*on_draw)(sui_widget_t *, struct sui_canvas *);
    void (*on_event)(sui_widget_t *, const struct sui_event *);
    void (*on_layout)(sui_widget_t *);
    void *user_data;

    const struct sui_widget_vtable *vtable;
    char  text[128];         /* 通用文本（标签/按钮/标题） */
    int   tag;               /* 任意用途（分组/子类型） */
};

struct sui_widget_vtable {
    const char *type_name;
    void (*draw)(sui_widget_t *, struct sui_canvas *);
    void (*measure)(sui_widget_t *);
    bool (*on_event)(sui_widget_t *, const struct sui_event *);
    void (*destroy)(sui_widget_t *);
};
typedef struct sui_widget_vtable sui_widget_vtable_t;

sui_widget_t *sui_widget_create(const sui_widget_vtable_t *vt, sui_widget_t *parent);
/* 分配 sizeo(sui_widget_t)+extra 字节（扩展控件用，避免越界覆盖相邻控件） */
sui_widget_t *sui_widget_create_ex(const sui_widget_vtable_t *vt, sui_widget_t *parent, size_t extra);
void sui_widget_destroy(sui_widget_t *w);
void sui_widget_destroy_recursive(sui_widget_t *w);
void sui_widget_add_child(sui_widget_t *parent, sui_widget_t *child);
void sui_widget_remove_child(sui_widget_t *parent, sui_widget_t *child);
void sui_widget_set_parent(sui_widget_t *w, sui_widget_t *new_parent);
void sui_widget_set_rect(sui_widget_t *w, int x, int y, int w_, int h_);
void sui_widget_set_pos(sui_widget_t *w, int x, int y);
void sui_widget_set_size(sui_widget_t *w, int w_, int h_);
void sui_widget_set_visible(sui_widget_t *w, bool v);
void sui_widget_set_enabled(sui_widget_t *w, bool e);
void sui_widget_set_text(sui_widget_t *w, const char *text);
void sui_widget_focus(sui_widget_t *w);
void sui_widget_blur(sui_widget_t *w);
sui_widget_t *sui_widget_focused(void);
void sui_widget_invalidate(sui_widget_t *w);
void sui_widget_invalidate_rect(sui_widget_t *w, int x, int y, int w_, int h_);
sui_widget_t *sui_widget_hit_test(sui_widget_t *root, int x, int y);
void sui_widget_to_screen(sui_widget_t *w, int *x, int *y);
void sui_widget_from_screen(sui_widget_t *w, int *x, int *y);

/* 布局 */
void sui_layout_apply(sui_widget_t *root);
void sui_widget_invalidate_layout(sui_widget_t *w);
void sui_widget_set_layout(sui_widget_t *w, const sui_layout_params_t *params);
void sui_widget_set_constraint(sui_widget_t *w, const sui_size_constraint_t *c);

/* =========================================================================
 * 8. 画布与绘制原语（规范 8）
 * ====================================================================== */
typedef struct sui_canvas {
    struct sui_window *win;
    uint32_t *pixels;       /* 离屏缓冲（xRGB32） */
    int      width, height;
    int      clip_x, clip_y, clip_w, clip_h;
    float    alpha;
} sui_canvas_t;

sui_canvas_t *sui_canvas_create(struct sui_window *win);
void sui_canvas_destroy(sui_canvas_t *c);
void sui_canvas_flush(sui_canvas_t *c, int x, int y, int w, int h);

void sui_canvas_set_pixel(sui_canvas_t *c, int x, int y, uint32_t color);
void sui_canvas_fill_rect(sui_canvas_t *c, int x, int y, int w, int h, uint32_t color);
void sui_canvas_draw_line(sui_canvas_t *c, int x0, int y0, int x1, int y1, uint32_t color, int thickness);
void sui_canvas_fill_rounded_rect(sui_canvas_t *c, int x, int y, int w, int h, int radius, uint32_t color);
void sui_canvas_stroke_rounded_rect(sui_canvas_t *c, int x, int y, int w, int h, int radius, int thickness, uint32_t color);
void sui_canvas_fill_circle(sui_canvas_t *c, int cx, int cy, int r, uint32_t color);
void sui_canvas_stroke_circle(sui_canvas_t *c, int cx, int cy, int r, int thickness, uint32_t color);
void sui_canvas_draw_shadow(sui_canvas_t *c, int x, int y, int w, int h, int radius, int blur, uint32_t color);
void sui_canvas_draw_text(sui_canvas_t *c, int x, int y, const char *text, int font_size, int weight, uint32_t color);
int  sui_canvas_measure_text(sui_canvas_t *c, const char *text, int font_size, int weight);
void sui_canvas_push_clip(sui_canvas_t *c, int x, int y, int w, int h);
void sui_canvas_pop_clip(sui_canvas_t *c);
void sui_canvas_draw_background(sui_canvas_t *c, const sui_widget_t *w, int radius, uint32_t bg, int shadow_blur, uint32_t shadow_color);
void sui_canvas_draw_focus_ring(sui_canvas_t *c, const sui_widget_t *w);

/* 内部：圆角四角填充 */
void sui__fill_corner(sui_canvas_t *c, int cx, int cy, int r, uint32_t color, int quad);

/* =========================================================================
 * 9. 事件系统（规范 13）
 * ====================================================================== */
typedef enum {
    SUI_EVENT_NONE = 0,
    SUI_EVENT_MOUSE_MOVE,
    SUI_EVENT_MOUSE_DOWN,
    SUI_EVENT_MOUSE_UP,
    SUI_EVENT_MOUSE_WHEEL,
    SUI_EVENT_KEY_DOWN,
    SUI_EVENT_KEY_UP,
    SUI_EVENT_KEY_CHAR,
    SUI_EVENT_FOCUS_IN,
    SUI_EVENT_FOCUS_OUT,
    SUI_EVENT_WINDOW_CLOSE,
    SUI_EVENT_CLICK,
    SUI_EVENT_VALUE_CHANGED,
    SUI_EVENT_TIMER
} sui_event_type_t;

typedef struct sui_event {
    sui_event_type_t type;
    uint64_t timestamp;
    bool    stopped;
    int     x, y, dx, dy, button;
    uint32_t key;
    uint32_t mods;
    char    ch;
    int     value;      /* 通用数值（slider 0-100 / checkbox 0,1,2） */
} sui_event_t;

void sui_event_stop(const sui_event_t *ev);
void sui_dispatch_event(sui_widget_t *root, const sui_event_t *ev);

/* =========================================================================
 * 10. 窗口（规范 12，精简）
 * ====================================================================== */
#define SUI_TITLEBAR_H  38
#define SUI_WIN_MIN_W   320
#define SUI_WIN_MIN_H   200

typedef struct sui_window {
    suki_window_t *wk;     /* 底层 libsuki_gui 窗口 */
    sui_widget_t  *root;   /* 根控件（覆盖整窗） */
    sui_widget_t  *overlay;/* 浮层根（对话框/菜单/通知），默认 hidden，绘制于最上层 */
    bool running;
    bool active;           /* 窗口是否处于活动（聚焦）状态，影响标题栏/交通灯样式 */
    sui_widget_t *focus;
    uint32_t bg;           /* 窗口背景色（RGB） */
    bool     has_titlebar;
    char     title[64];
} sui_window_t;

int  sui_init(sui_window_t *win);
void sui_shutdown(void);
sui_window_t *sui_create_window(const char *title, int x, int y, int w, int h, bool titlebar);
void sui_destroy_window(sui_window_t *win);
void sui_window_set_bg(sui_window_t *win, uint32_t rgb);
void sui_window_set_title(sui_window_t *win, const char *title);
void sui_render_window(sui_window_t *win);
void sui_window_present(sui_window_t *win, int x, int y, int w, int h);
bool sui_window_poll_event(sui_window_t *win, sui_event_t *out);
void sui_window_set_event_port(sui_window_t *win);
/* 单帧：拉事件 + 命中/焦点分发；返回是否仍有事件待处理 */
bool sui_window_step(sui_window_t *win, sui_event_t *out);

/* =========================================================================
 * 11. 控件（规范 9，覆盖核心控件集）
 * ====================================================================== */
typedef enum {
    SUI_BTN_PRIMARY, SUI_BTN_SECONDARY, SUI_BTN_DANGER, SUI_BTN_GHOST, SUI_BTN_ICON
} sui_button_variant_t;
typedef enum { SUI_BTN_SIZE_SM = 26, SUI_BTN_SIZE_MD = 32, SUI_BTN_SIZE_LG = 38 } sui_button_size_t;

typedef struct sui_button {
    sui_widget_t base;
    sui_button_variant_t variant;
    sui_button_size_t size;
    void (*on_click)(sui_widget_t *, void *);
    void *user;
} sui_button_t;
sui_button_t *sui_button_create(sui_widget_t *parent, sui_button_variant_t v, sui_button_size_t s, const char *text);
void sui_button_set_text(sui_button_t *b, const char *text);
void sui_button_on_click(sui_button_t *b, void (*fn)(sui_widget_t *, void *), void *user);

typedef struct sui_label {
    sui_widget_t base;
    int font_size;
    int weight;
    uint32_t color;     /* 0 = 主题 text_primary */
} sui_label_t;
sui_label_t *sui_label_create(sui_widget_t *parent, const char *text);
void sui_label_set_font(sui_label_t *l, int size, int weight);

typedef struct sui_checkbox {
    sui_widget_t base;
    bool checked;
    bool indeterminate;
    void (*on_change)(sui_widget_t *, bool, void *);
    void *user;
} sui_checkbox_t;
sui_checkbox_t *sui_checkbox_create(sui_widget_t *parent, const char *label, bool checked);
void sui_checkbox_set_checked(sui_checkbox_t *c, bool checked);
void sui_checkbox_on_change(sui_checkbox_t *c, void (*fn)(sui_widget_t *, bool, void *), void *user);

typedef struct sui_switch {
    sui_widget_t base;
    bool on;
    void (*on_change)(sui_widget_t *, bool, void *);
    void *user;
} sui_switch_t;
sui_switch_t *sui_switch_create(sui_widget_t *parent, bool on);
void sui_switch_set_on(sui_switch_t *s, bool on);
void sui_switch_on_change(sui_switch_t *s, void (*fn)(sui_widget_t *, bool, void *), void *user);

typedef struct sui_slider {
    sui_widget_t base;
    int min, max, value, step;
    bool show_value;
    void (*on_change)(sui_widget_t *, int, void *);
    void *user;
} sui_slider_t;
sui_slider_t *sui_slider_create(sui_widget_t *parent, int min, int max, int value);
void sui_slider_set_value(sui_slider_t *s, int value);
void sui_slider_on_change(sui_slider_t *s, void (*fn)(sui_widget_t *, int, void *), void *user);

typedef struct sui_progress {
    sui_widget_t base;
    int value;       /* 0-100 */
    int thickness;
    uint32_t color;
    bool indeterminate;
} sui_progress_t;
sui_progress_t *sui_progress_create(sui_widget_t *parent, int value);
void sui_progress_set_value(sui_progress_t *p, int value);

typedef struct sui_card {
    sui_widget_t base;
    char body[256];
} sui_card_t;
sui_card_t *sui_card_create(sui_widget_t *parent, const char *title, const char *body);
void sui_card_set_body(sui_card_t *c, const char *body);

/* 输入框（单行） */
typedef struct sui_input {
    sui_widget_t base;
    char text[256];
    int  cursor;
    char placeholder[128];
    bool readonly;
    void (*on_change)(sui_widget_t *, const char *, void *);
    void (*on_submit)(sui_widget_t *, const char *, void *);
    void *user;
} sui_input_t;
sui_input_t *sui_input_create(sui_widget_t *parent, const char *placeholder);
void sui_input_set_text(sui_input_t *i, const char *text);
const char *sui_input_text(const sui_input_t *i);

/* =========================================================================
 * 12. 字体后端（委托 fontsrv 离屏渲染：libsui 不再自带 FreeType 合成器）
 * ====================================================================== */
int  sui_font_init(void);                       /* 加载默认字体（中/英 fallback） */
void sui_font_set_default(const char *path);   /* 覆盖默认字体路径 */
bool sui_font_ready(void);                      /* 字体是否就绪（否则降级 ASCII 点阵） */
int  sui_font_draw(sui_canvas_t *c, int x, int y, const char *text, int size, uint32_t color);
int  sui_font_measure(const char *text, int size);
void sui_font_unregister_all(void);              /* 窗口销毁时解除画布注册（让 fontsrv 解映射） */

/* =========================================================================
 * 13. 扩展控件（补全控件库，复刻 iSuki UI 概念稿）
 * ====================================================================== */
/* 容器：面板 / 分区 */
typedef struct sui_panel {
    sui_widget_t base;
    char title[64];
} sui_panel_t;
sui_panel_t *sui_panel_create(sui_widget_t *parent, const char *title);

typedef struct sui_section {
    sui_widget_t base;
    char title[48];
} sui_section_t;
sui_section_t *sui_section_create(sui_widget_t *parent, const char *title);

/* 侧边栏导航项 */
typedef struct sui_navitem {
    sui_widget_t base;
    char label[48];
    char icon;          /* 单字符图标（无 SVG 资产时用字符占位） */
    char count[8];
    bool active;
    void (*on_click)(sui_widget_t *, void *);
    void *user;
} sui_navitem_t;
sui_navitem_t *sui_navitem_create(sui_widget_t *parent, const char *label, char icon, const char *count);
void sui_navitem_set_active(sui_navitem_t *n, bool a);
void sui_navitem_on_click(sui_navitem_t *n, void (*fn)(sui_widget_t *, void *), void *user);

/* 单选 */
typedef struct sui_radio {
    sui_widget_t base;
    char label[48];
    int  group;
    bool checked;
    void (*on_change)(sui_widget_t *, bool, void *);
    void *user;
} sui_radio_t;
sui_radio_t *sui_radio_create(sui_widget_t *parent, const char *label, int group, bool checked);
void sui_radio_set_checked(sui_radio_t *r, bool c);
void sui_radio_on_change(sui_radio_t *r, void (*fn)(sui_widget_t *, bool, void *), void *user);

/* 分段 */
typedef struct sui_segmented {
    sui_widget_t base;
    char segments[8][24];
    int  count;
    int  selected;
    void (*on_change)(sui_widget_t *, int, void *);
    void *user;
} sui_segmented_t;
sui_segmented_t *sui_segmented_create(sui_widget_t *parent, const char **items, int count);
void sui_segmented_set_selected(sui_segmented_t *s, int idx);
void sui_segmented_on_change(sui_segmented_t *s, void (*fn)(sui_widget_t *, int, void *), void *user);

/* 标签页 */
typedef struct sui_tabs {
    sui_widget_t base;
    char tabs[8][24];
    int  count;
    int  selected;
    void (*on_change)(sui_widget_t *, int, void *);
    void *user;
} sui_tabs_t;
sui_tabs_t *sui_tabs_create(sui_widget_t *parent, const char **items, int count);
void sui_tabs_set_selected(sui_tabs_t *t, int idx);
void sui_tabs_on_change(sui_tabs_t *t, void (*fn)(sui_widget_t *, int, void *), void *user);

/* 列表项 / 列表 */
typedef struct sui_list_item {
    char avatar_text[4];
    uint32_t avatar_a, avatar_b;   /* 头像渐变两端色 */
    char title[64];
    char subtitle[64];
    char badge[16];
    int  badge_kind;              /* 0 accent / 1 gray / 2 green */
} sui_list_item_t;
typedef struct sui_list {
    sui_widget_t base;
    sui_list_item_t items[32];
    int count;
    int selected;
    char filter[64];
    void (*on_select)(sui_widget_t *, int, void *);
    void *user;
} sui_list_t;
sui_list_t *sui_list_create(sui_widget_t *parent);
int  sui_list_add(sui_list_t *l, const sui_list_item_t *item);
void sui_list_set_filter(sui_list_t *l, const char *q);
void sui_list_on_select(sui_list_t *l, void (*fn)(sui_widget_t *, int, void *), void *user);

/* 提示条 */
typedef enum { SUI_ALERT_INFO = 0, SUI_ALERT_WARN, SUI_ALERT_OK } sui_alert_kind_t;
typedef struct sui_alert {
    sui_widget_t base;
    sui_alert_kind_t kind;
    char title[64];
    char body[192];
} sui_alert_t;
sui_alert_t *sui_alert_create(sui_widget_t *parent, sui_alert_kind_t kind, const char *title, const char *body);

/* 头像 */
typedef struct sui_avatar {
    sui_widget_t base;
    char text[4];
    int  size;
    uint32_t grad_a, grad_b;
} sui_avatar_t;
sui_avatar_t *sui_avatar_create(sui_widget_t *parent, const char *text, int size);
void sui_avatar_set_gradient(sui_avatar_t *a, uint32_t ga, uint32_t gb);

/* 徽标 */
typedef enum { SUI_BADGE_ACCENT = 0, SUI_BADGE_GRAY, SUI_BADGE_GREEN } sui_badge_kind_t;
typedef struct sui_badge {
    sui_widget_t base;
    sui_badge_kind_t kind;
    char text[16];
} sui_badge_t;
sui_badge_t *sui_badge_create(sui_widget_t *parent, const char *text, sui_badge_kind_t kind);

/* 多行输入 */
typedef struct sui_textarea {
    sui_widget_t base;
    char text[512];
    int  cursor;
    char placeholder[64];
    void (*on_change)(sui_widget_t *, const char *, void *);
    void *user;
} sui_textarea_t;
sui_textarea_t *sui_textarea_create(sui_widget_t *parent, const char *placeholder);
void sui_textarea_set_text(sui_textarea_t *t, const char *text);
const char *sui_textarea_text(const sui_textarea_t *t);

/* 下拉选择 */
typedef struct sui_select {
    sui_widget_t base;
    char options[12][32];
    int  count;
    int  selected;
    bool open;
    void (*on_change)(sui_widget_t *, int, void *);
    void *user;
} sui_select_t;
sui_select_t *sui_select_create(sui_widget_t *parent, const char **opts, int count);
void sui_select_set_selected(sui_select_t *s, int idx);
void sui_select_on_change(sui_select_t *s, void (*fn)(sui_widget_t *, int, void *), void *user);

/* 浮层（overlay）与对话框 / 通知 */
sui_widget_t *sui_window_overlay(sui_window_t *win);
void sui_overlay_clear(sui_widget_t *overlay);
typedef enum { SUI_TOAST_INFO = 0, SUI_TOAST_SUCCESS, SUI_TOAST_WARN } sui_toast_kind_t;
void sui_toast_show(sui_window_t *win, sui_toast_kind_t kind, const char *title, const char *body);
void sui_dialog_show(sui_window_t *win, const char *title, const char *body,
                     const char *ok_label, const char *cancel_label,
                     void (*on_ok)(void *), void *user);
void sui_dialog_close(sui_window_t *win);

#ifdef __cplusplus
}
#endif
#endif /* SUI_H */
