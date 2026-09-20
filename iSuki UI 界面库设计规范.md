# iSuki UI 界面库设计规范

> **文档版本**：v1.0  
> **适用系统**：SukiOS  
> **目标**：作为 `libsui` 界面库的完整实现依据，AI 或开发者可直接据此编写 C 代码  
> **依赖**：`libsuki_gui`（窗口客户端）、`fontsrv`（字体服务）、`display_server`（合成器）  
> **约定**：本文所有尺寸单位为逻辑像素（px），颜色为 ARGB8888 或 `0xAARRGGBB`

---

## 目录

1. [设计哲学](#1-设计哲学)
2. [命名与代码规范](#2-命名与代码规范)
3. [设计令牌 Design Tokens](#3-设计令牌-design-tokens)
4. [主题系统](#4-主题系统)
5. [材质系统](#5-材质系统)
6. [布局与网格](#6-布局与网格)
7. [控件基类规范](#7-控件基类规范)
8. [基础绘制原语](#8-基础绘制原语)
9. [组件规范](#9-组件规范)
10. [交互与状态机](#10-交互与状态机)
11. [动效规范](#11-动效规范)
12. [窗口与桌面规范](#12-窗口与桌面规范)
13. [事件系统](#13-事件系统)
14. [API 汇总](#14-api-汇总)
15. [文件结构与实现约束](#15-文件结构与实现约束)
16. [附录](#16-附录)

---

## 1. 设计哲学

### 1.1 风格定位

**iSuki UI** = macOS 的精致与留白 + Windows Fluent 的效率与强调色。

| 维度 | 采纳 macOS | 采纳 Windows |
|---|---|---|
| 桌面 | 顶部全局菜单栏 | 底部居中任务栏 |
| 窗口 | 左上交通灯、圆角阴影 | 右上 Snap、控制按钮 |
| 材质 | Vibrancy、毛玻璃 | Mica、Acrylic、Smoke |
| 控件 | 大圆角、留白 | Fluent 状态反馈 |
| 动效 | 弹簧、流畅 | 快速、方向感 |
| 强调色 | 系统级统一 | 用户可自定义 |

### 1.2 七条铁律

1. **内容优先**：窗口装饰退后，材质表达层级。
2. **材质即层级**：Mica 窗口 / Acrylic 浮层 / Smoke 菜单 / Solid 兜底。
3. **统一圆角与间距**：所有控件共用 4px 网格与圆角阶梯。
4. **强调色贯穿**：按钮、焦点环、选中态、开关、进度条同一强调色。
5. **动效有方向、可打断**：进入 180–240ms，退出 120–150ms。
6. **键盘与鼠标等价**：所有操作可 Tab / 方向键 / Enter 完成。
7. **默认可配置**：浅色/深色、强调色、材质强度、圆角、密度、按钮布局。

### 1.3 硬性约束

- 界面库**不依赖内核**，只依赖 `libsuki_gui` + `fontsrv` IPC。
- 所有渲染在**应用离屏缓冲**中完成，`display_server` 只做合成。
- 每一帧的最终提交必须走 `suki_flush(win, x, y, w, h)`。
- 单条 OOL 消息上限 16 页（64 KiB），大窗口必须分片提交。
- 所有控件必须支持**键盘焦点**与**禁用状态**。
- 所有颜色必须来自主题结构体，**禁止硬编码颜色**（除交通灯、语义色）。

---

## 2. 命名与代码规范

### 2.1 前缀

| 层级 | 前缀 | 示例 |
|---|---|---|
| 库总入口 | `sui_` | `sui_init` |
| 控件 | `sui_<控件>_` | `sui_button_create` |
| 内部函数 | `sui__`（双下划线） | `sui__blit_rounded` |
| 宏 | `SUI_` | `SUI_RADIUS_WINDOW` |
| 枚举 | `sui_<类型>_t` | `sui_material_t` |
| 结构体 | `sui_<名称>_t` | `sui_theme_t` |

### 2.2 头文件保护

```c
#ifndef SUI_WIDGET_H
#define SUI_WIDGET_H
...
#endif /* SUI_WIDGET_H */
```

### 2.3 禁止事项

- 禁止在头文件中 `#include` 其他库头文件（除 `<stdint.h>`、`<stdbool.h>`）。
- 禁止在控件实现中直接调用 `sys_*` 系统调用。
- 禁止使用浮点数做像素计算（用 `int` 或 `fixed-point`）。
- 禁止动态内存分配超过 4 KiB 的连续块（大窗口用 mmap）。

---

## 3. 设计令牌 Design Tokens

### 3.1 颜色令牌（浅色主题）

```c
/* ---- 基底 ---- */
#define SUI_L_MICA_BASE         0xFFF3F3F3  /* 窗口壁纸采样基底 */
#define SUI_L_WINDOW_BG         0xCCFFFFFF  /* 窗口背景 ARGB */
#define SUI_L_CARD_BG           0xB3FFFFFF  /* 卡片背景 */
#define SUI_L_SIDEBAR_BG        0x8CFFFFFF  /* 侧边栏 */
#define SUI_L_MENU_BG           0xD9FCFCFE  /* 菜单浮层 */
#define SUI_L_TOAST_BG          0xF0FCFCFE  /* 通知 */

/* ---- 描边 ---- */
#define SUI_L_STROKE            0x1A000000  /* 主描边 10% 黑 */
#define SUI_L_STROKE_SOFT       0x0F000000  /* 弱描边 6% */
#define SUI_L_DIVIDER           0x14000000  /* 分隔线 */

/* ---- 文字 ---- */
#define SUI_L_TEXT_PRIMARY      0xFF1D1D1F
#define SUI_L_TEXT_SECONDARY    0xFF6E6E73
#define SUI_L_TEXT_TERTIARY     0xFFA1A1A6
#define SUI_L_TEXT_DISABLED     0x611D1D1F  /* 38% */

/* ---- 语义色 ---- */
#define SUI_L_ACCENT            0xFF0A84FF  /* 默认蓝，可被主题覆盖 */
#define SUI_L_ACCENT_HOVER      0xFF2B95FF
#define SUI_L_ACCENT_PRESSED    0xFF0060DF
#define SUI_L_DANGER            0xFFFF453A
#define SUI_L_WARNING           0xFFFF9F0A
#define SUI_L_SUCCESS           0xFF30D158

/* ---- 交通灯 ---- */
#define SUI_TRAFFIC_CLOSE       0xFFFF5F57
#define SUI_TRAFFIC_MIN         0xFFFEBC2E
#define SUI_TRAFFIC_MAX         0xFF28C840
```

### 3.2 颜色令牌（深色主题）

```c
#define SUI_D_MICA_BASE         0xFF1C1C1E
#define SUI_D_WINDOW_BG         0xE01C1C1E
#define SUI_D_CARD_BG           0x0BFFFFFF
#define SUI_D_SIDEBAR_BG        0x08FFFFFF
#define SUI_D_MENU_BG           0xDB2E2E2E
#define SUI_D_TOAST_BG          0xEB303030

#define SUI_D_STROKE            0x1AFFFFFF
#define SUI_D_STROKE_SOFT       0x0FFFFFFF
#define SUI_D_DIVIDER           0x14FFFFFF

#define SUI_D_TEXT_PRIMARY      0xFFF5F5F7
#define SUI_D_TEXT_SECONDARY    0xFF98989D
#define SUI_D_TEXT_TERTIARY     0xFF6E6E73
#define SUI_D_TEXT_DISABLED     0x61F5F5F7

#define SUI_D_ACCENT            0xFF0A84FF
#define SUI_D_ACCENT_HOVER      0xFF2B95FF
#define SUI_D_ACCENT_PRESSED    0xFF0060DF
#define SUI_D_DANGER            0xFFFF453A
#define SUI_D_WARNING           0xFFFF9F0A
#define SUI_D_SUCCESS           0xFF30D158
```

### 3.3 强调色预设

```c
typedef enum {
    SUI_ACCENT_BLUE   = 0xFF0A84FF,
    SUI_ACCENT_PURPLE = 0xFF7B5CFF,
    SUI_ACCENT_PINK   = 0xFFFF375F,
    SUI_ACCENT_RED    = 0xFFFF453A,
    SUI_ACCENT_ORANGE = 0xFFFF9F0A,
    SUI_ACCENT_GREEN  = 0xFF30D158,
    SUI_ACCENT_CYAN   = 0xFF64D2FF,
    SUI_ACCENT_YELLOW = 0xFFFFD60A,
    SUI_ACCENT_CUSTOM = 0x00000000  /* 使用自定义值 */
} sui_accent_t;
```

### 3.4 间距令牌

基数 **4px**，只允许以下取值：

```c
#define SUI_SPACE_0     0
#define SUI_SPACE_1     4
#define SUI_SPACE_2     8
#define SUI_SPACE_3     12
#define SUI_SPACE_4     16
#define SUI_SPACE_5     20
#define SUI_SPACE_6     24
#define SUI_SPACE_8     32
#define SUI_SPACE_10    40
#define SUI_SPACE_12    48
#define SUI_SPACE_16    64
```

**语义映射**：

| 场景 | 值 |
|---|---|
| 图标与文字间距 | 8 |
| 按钮内边距（左右） | 14 |
| 卡片内边距 | 16 |
| 面板内边距 | 16 |
| 内容区左右内边距 | 30 |
| 页面顶部内边距 | 26 |
| 分组之间 | 30 |
| 标题与内容之间 | 10 |

### 3.5 圆角令牌

```c
#define SUI_RADIUS_NONE     0
#define SUI_RADIUS_SM       4    /* 复选框、徽标 */
#define SUI_RADIUS_CTRL     6    /* 按钮、输入框、下拉 */
#define SUI_RADIUS_MD       8    /* 卡片、菜单、Toast、对话框 */
#define SUI_RADIUS_LG       10   /* 面板、浮层 */
#define SUI_RADIUS_WINDOW   12   /* 窗口 */
#define SUI_RADIUS_FULL     9999 /* 胶囊、开关、头像 */
```

**映射规则**：

| 控件 | 圆角 |
|---|---|
| 窗口 | 12 |
| 面板 / 大浮层 | 10 |
| 卡片 / 菜单 / Toast / 对话框 | 8 |
| 按钮 / 输入框 / 下拉 / 标签页 | 6 |
| 复选框 / 徽标 / 小标签 | 4 |
| 开关 / 头像 / 胶囊按钮 | 9999 |

### 3.6 字体令牌

**字体族**（按优先级）：

1. 中文：`ResourceHanRoundedCN-Medium`（已嵌入）
2. 英文/数字：`Inter` 或 `Segoe UI Variable`
3. 等宽：`JetBrains Mono`

**字号阶梯**：

```c
typedef enum {
    SUI_FONT_CAPTION    = 11,
    SUI_FONT_FOOTNOTE   = 12,
    SUI_FONT_BODY       = 13,
    SUI_FONT_SUBTITLE   = 15,
    SUI_FONT_TITLE      = 20,
    SUI_FONT_LARGE      = 24,
    SUI_FONT_DISPLAY    = 28
} sui_font_size_t;
```

**字重**：

```c
typedef enum {
    SUI_WEIGHT_REGULAR  = 400,
    SUI_WEIGHT_MEDIUM   = 500,
    SUI_WEIGHT_SEMIBOLD = 600,
    SUI_WEIGHT_BOLD     = 700
} sui_font_weight_t;
```

**行高**：`font_size * 1.4`（向上取整）。  
**字距**：标题 `-0.01em`，正文 `0`，全大写标签 `+0.09em`。

### 3.7 阴影令牌

阴影以多层半透明矩形模拟。每层参数：`offset_x, offset_y, blur, color`。

```c
typedef struct {
    int      dx;
    int      dy;
    int      blur;
    uint32_t color;
} sui_shadow_layer_t;

typedef struct {
    const sui_shadow_layer_t *layers;
    int                       count;
} sui_shadow_t;

/* 预设 */
static const sui_shadow_layer_t SHADOW_CARD[] = {
    {0, 2, 8, 0x14000000}   /* 8% 黑 */
};
static const sui_shadow_layer_t SHADOW_MENU[] = {
    {0, 8, 24, 0x24000000},
    {0, 2, 6,  0x14000000}
};
static const sui_shadow_layer_t SHADOW_WINDOW_ACTIVE[] = {
    {0, 16, 48, 0x38000000},
    {0, 4,  12, 0x1A000000}
};
static const sui_shadow_layer_t SHADOW_DIALOG[] = {
    {0, 24, 64, 0x47000000},
    {0, 8,  20, 0x24000000}
};
```

| 层级 | 用途 |
|---|---|
| `SUI_SHADOW_CARD` | 卡片悬停 |
| `SUI_SHADOW_MENU` | 菜单、下拉、通知 |
| `SUI_SHADOW_WINDOW_ACTIVE` | 活动窗口 |
| `SUI_SHADOW_WINDOW_INACTIVE` | 非活动窗口（强度 × 0.5） |
| `SUI_SHADOW_DIALOG` | 对话框 |

### 3.8 动效令牌

```c
typedef enum {
    SUI_DUR_INSTANT   = 0,
    SUI_DUR_FAST      = 100,
    SUI_DUR_HOVER     = 120,
    SUI_DUR_STANDARD  = 180,
    SUI_DUR_ENTER     = 240,
    SUI_DUR_EXIT      = 150,
    SUI_DUR_TOAST     = 300,
    SUI_DUR_PAGE      = 280
} sui_duration_t;

typedef enum {
    SUI_EASE_STANDARD,  /* cubic-bezier(0.2, 0, 0, 1) 通用 */
    SUI_EASE_DECEL,     /* cubic-bezier(0, 0, 0, 1)   进入 */
    SUI_EASE_ACCEL,     /* cubic-bezier(0.3, 0, 1, 1) 退出 */
    SUI_EASE_SPRING,    /* 阻尼弹簧，用于窗口 */
    SUI_EASE_LINEAR
} sui_easing_t;

float sui_ease(sui_easing_t e, float t);
```

**缓动函数实现**：

```c
float sui_ease(sui_easing_t e, float t){
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    switch (e) {
    case SUI_EASE_STANDARD:
        /* 近似 cubic-bezier(0.2,0,0,1) */
        return 1.0f - powf(1.0f - t, 3.0f);
    case SUI_EASE_DECEL:
        return 1.0f - powf(1.0f - t, 4.0f);
    case SUI_EASE_ACCEL:
        return t * t * t;
    case SUI_EASE_SPRING: {
        /* 阻尼弹簧，频率 14，阻尼 0.85 */
        float f = 14.0f, d = 0.85f;
        return 1.0f - expf(-d * f * t) * cosf(f * t);
    }
    case SUI_EASE_LINEAR:
    default:
        return t;
    }
}
```

### 3.9 Z 序令牌

```c
#define SUI_Z_DESKTOP       0
#define SUI_Z_WINDOW_MIN    100
#define SUI_Z_WINDOW_MAX    999
#define SUI_Z_DOCK          1000
#define SUI_Z_MENUBAR       1010
#define SUI_Z_MENU          2000
#define SUI_Z_TOOLTIP       2500
#define SUI_Z_TOAST         3000
#define SUI_Z_DIALOG        3500
#define SUI_Z_OVERLAY       4000
```

---

## 4. 主题系统

### 4.1 主题结构体

```c
typedef struct {
    /* 标识 */
    const char *name;
    bool        is_dark;

    /* 基底 */
    uint32_t mica_base;
    uint32_t window_bg;
    uint32_t card_bg;
    uint32_t sidebar_bg;
    uint32_t menu_bg;
    uint32_t toast_bg;

    /* 描边 */
    uint32_t stroke;
    uint32_t stroke_soft;
    uint32_t divider;

    /* 文字 */
    uint32_t text_primary;
    uint32_t text_secondary;
    uint32_t text_tertiary;
    uint32_t text_disabled;

    /* 强调色 */
    uint32_t accent;
    uint32_t accent_hover;
    uint32_t accent_pressed;
    uint32_t accent_soft;      /* 16% 强调色，用于选中背景 */

    /* 语义色 */
    uint32_t danger;
    uint32_t warning;
    uint32_t success;

    /* 控件状态 */
    uint32_t surface;          /* 控件默认底 */
    uint32_t surface_hover;
    uint32_t surface_active;
    uint32_t input_bg;
    uint32_t switch_off;
    uint32_t box_border;
    uint32_t range_track;
    uint32_t range_thumb;

    /* 全局设置 */
    int      radius_window;
    int      radius_control;
    int      titlebar_height;
    float    material_opacity; /* 0.0 - 1.0 */
    int      density;          /* 0 紧凑 / 50 舒适 / 100 宽松 */
} sui_theme_t;
```

### 4.2 主题 API

```c
/* 获取当前主题（只读） */
const sui_theme_t *sui_theme(void);

/* 设置主题 */
void sui_theme_set(const sui_theme_t *theme);

/* 切换浅色 / 深色 */
void sui_theme_set_dark(bool dark);
bool sui_theme_is_dark(void);

/* 设置强调色（自动计算 hover/pressed/soft） */
void sui_theme_set_accent(sui_accent_t preset, uint32_t custom);

/* 全局配置 */
void sui_theme_set_radius_window(int r);
void sui_theme_set_radius_control(int r);
void sui_theme_set_material_opacity(float opacity);
void sui_theme_set_density(int density);

/* 预设主题 */
extern const sui_theme_t SUI_THEME_LIGHT;
extern const sui_theme_t SUI_THEME_DARK;
```

### 4.3 强调色派生规则

给定 `accent` 基色，派生：

```c
static uint32_t derive_hover(uint32_t c){
    /* HSL 亮度 +8% */
    return hsl_adjust(c, 0, 0, +8);
}
static uint32_t derive_pressed(uint32_t c){
    /* HSL 亮度 -8% */
    return hsl_adjust(c, 0, 0, -8);
}
static uint32_t derive_soft(uint32_t c){
    /* alpha 设为 16% */
    return (c & 0x00FFFFFF) | 0x29000000;
}
```

### 4.4 主题变更通知

```c
typedef void (*sui_theme_changed_fn)(void *user);
void sui_theme_on_change(sui_theme_changed_fn fn, void *user);
```

所有控件在绘制时**必须**调用 `sui_theme()` 读取当前值，不缓存颜色。

---

## 5. 材质系统

### 5.1 材质枚举

```c
typedef enum {
    SUI_MATERIAL_SOLID   = 0,  /* 纯色，无透明 */
    SUI_MATERIAL_SMOKE   = 1,  /* 纯色半透明，无模糊 */
    SUI_MATERIAL_MICA    = 2,  /* 窗口基底，从壁纸采样 */
    SUI_MATERIAL_ACRYLIC = 3,  /* 浮层，模糊 + 噪声 */
    SUI_MATERIAL_VIBRANT = 4   /* 强模糊，用于全屏覆盖 */
} sui_material_t;
```

### 5.2 材质参数

```c
typedef struct {
    sui_material_t type;
    float          opacity;      /* 0.0 - 1.0 */
    int            blur_radius;  /* 像素，0 = 无模糊 */
    int            noise_amount; /* 0 - 255，0 = 无噪声 */
    bool           sample_wallpaper; /* Mica 专用 */
    uint32_t       tint;         /* 叠加色调 */
} sui_material_desc_t;

/* 预设 */
#define SUI_MAT_MICA_WINDOW  (sui_material_desc_t){ \
    .type = SUI_MATERIAL_MICA, .opacity = 0.88f, \
    .blur_radius = 40, .noise_amount = 8, .sample_wallpaper = true }

#define SUI_MAT_ACRYLIC_MENU (sui_material_desc_t){ \
    .type = SUI_MATERIAL_ACRYLIC, .opacity = 0.86f, \
    .blur_radius = 28, .noise_amount = 12 }

#define SUI_MAT_SMOKE_TOAST  (sui_material_desc_t){ \
    .type = SUI_MATERIAL_SMOKE, .opacity = 0.92f, \
    .blur_radius = 0, .noise_amount = 0 }
```

### 5.3 材质降级策略

合成器能力不足时，按以下顺序降级：

| 目标材质 | 降级 1 | 降级 2 | 降级 3 |
|---|---|---|---|
| Vibrant | Acrylic | Mica | Solid |
| Acrylic | Mica | Smoke | Solid |
| Mica | Smoke | Solid | Solid |
| Smoke | Solid | Solid | Solid |

**降级 1**：`blur_radius = 0`，保留透明度。  
**降级 2**：`opacity = 1.0`，保留壁纸色调。  
**降级 3**：纯色 `window_bg`。

### 5.4 材质应用 API

```c
/* 设置窗口材质 */
void suki_set_window_material(suki_window_t *win,
                              const sui_material_desc_t *desc);

/* 查询合成器能力 */
typedef struct {
    bool supports_blur;
    bool supports_alpha;
    bool supports_wallpaper_sample;
    bool supports_rounded_corner;
    bool supports_shadow;
} sui_compositor_caps_t;

const sui_compositor_caps_t *sui_compositor_caps(void);
```

### 5.5 降级实现

```c
static void resolve_material(const sui_material_desc_t *req,
                             sui_material_desc_t *out){
    const sui_compositor_caps_t *caps = sui_compositor_caps();
    *out = *req;

    if (!caps->supports_blur) {
        out->blur_radius = 0;
    }
    if (!caps->supports_alpha) {
        out->opacity = 1.0f;
    }
    if (!caps->supports_wallpaper_sample) {
        out->sample_wallpaper = false;
        out->tint = sui_theme()->mica_base;
    }
    if (out->opacity >= 1.0f && out->blur_radius == 0) {
        out->type = SUI_MATERIAL_SOLID;
    }
}
```

---

## 6. 布局与网格

### 6.1 布局容器

```c
typedef enum {
    SUI_LAYOUT_ABSOLUTE = 0,  /* 绝对定位 */
    SUI_LAYOUT_FLOW,          /* 流式，按主轴排列 */
    SUI_LAYOUT_GRID,          /* 网格 */
    SUI_LAYOUT_DOCK           /* 停靠，边缘吸附 */
} sui_layout_t;

typedef enum {
    SUI_AXIS_HORIZONTAL = 0,
    SUI_AXIS_VERTICAL
} sui_axis_t;

typedef enum {
    SUI_ALIGN_START = 0,
    SUI_ALIGN_CENTER,
    SUI_ALIGN_END,
    SUI_ALIGN_STRETCH
} sui_align_t;

typedef enum {
    SUI_JUSTIFY_START = 0,
    SUI_JUSTIFY_CENTER,
    SUI_JUSTIFY_END,
    SUI_JUSTIFY_SPACE_BETWEEN,
    SUI_JUSTIFY_SPACE_AROUND
} sui_justify_t;
```

### 6.2 布局参数

```c
typedef struct {
    sui_layout_t  type;
    sui_axis_t    axis;
    int           gap;         /* 子项间距 */
    int           padding[4];  /* 上右下左 */
    sui_align_t   align;       /* 交叉轴对齐 */
    sui_justify_t justify;     /* 主轴对齐 */
    int           columns;     /* Grid 列数 */
    int           row_gap;     /* Grid 行间距 */
} sui_layout_params_t;

/* 常用预设 */
#define SUI_LAYOUT_ROW(g) \
    (sui_layout_params_t){ .type=SUI_LAYOUT_FLOW, .axis=SUI_AXIS_HORIZONTAL, .gap=(g), .align=SUI_ALIGN_CENTER }
#define SUI_LAYOUT_COL(g) \
    (sui_layout_params_t){ .type=SUI_LAYOUT_FLOW, .axis=SUI_AXIS_VERTICAL, .gap=(g) }
```

### 6.3 布局计算

```c
/* 重新布局子树 */
void sui_layout_apply(sui_widget_t *root);

/* 手动触发单控件布局 */
void sui_widget_invalidate_layout(sui_widget_t *w);
```

布局算法：

1. 从根控件开始，递归计算。
2. 每个容器先测量子项 `preferred_size`。
3. 按主轴累加，超过容器尺寸时按 `flex` 收缩或换行。
4. 交叉轴按 `align` 对齐。
5. 写入子项 `x, y, w, h`，标记 `dirty = true`。

### 6.4 尺寸约束

```c
typedef struct {
    int min_w, max_w;
    int min_h, max_h;
    int preferred_w, preferred_h;
    int flex;  /* 0 = 固定，>0 按比例分配剩余空间 */
} sui_size_constraint_t;

void sui_widget_set_constraint(sui_widget_t *w,
                               const sui_size_constraint_t *c);
```

### 6.5 响应式断点

| 断点 | 宽度 | 布局 |
|---|---|---|
| Compact | < 600 | 侧边栏收起为图标条 |
| Regular | 600–900 | 标准布局 |
| Wide | > 900 | 侧边栏加宽，双栏内容 |

```c
typedef enum {
    SUI_BREAKPOINT_COMPACT = 0,
    SUI_BREAKPOINT_REGULAR,
    SUI_BREAKPOINT_WIDE
} sui_breakpoint_t;

sui_breakpoint_t sui_breakpoint_for_width(int width);
```

---

## 7. 控件基类规范

### 7.1 控件结构体

```c
typedef struct sui_widget sui_widget_t;

struct sui_widget {
    /* 树结构 */
    sui_widget_t *parent;
    sui_widget_t *first_child;
    sui_widget_t *last_child;
    sui_widget_t *prev_sibling;
    sui_widget_t *next_sibling;
    int           child_count;

    /* 几何 */
    int x, y, w, h;             /* 相对父控件 */
    int abs_x, abs_y;           /* 绝对坐标，布局后缓存 */
    sui_size_constraint_t constraint;

    /* 状态 */
    bool visible;
    bool enabled;
    bool focusable;
    bool focused;
    bool hovered;
    bool pressed;
    bool dirty;                 /* 需要重绘 */

    /* 布局 */
    sui_layout_params_t layout;

    /* 外观 */
    sui_material_t      material;
    float               opacity;
    int                 corner_radius;
    const sui_shadow_t *shadow;
    uint32_t            bg_color;   /* 0 = 使用主题默认 */

    /* 回调 */
    void (*on_draw)  (sui_widget_t *, sui_canvas_t *);
    void (*on_event) (sui_widget_t *, const sui_event_t *);
    void (*on_layout)(sui_widget_t *);
    void *user_data;

    /* 内部 */
    const struct sui_widget_vtable *vtable;
};
```

### 7.2 虚函数表

```c
struct sui_widget_vtable {
    const char *type_name;
    void (*draw)      (sui_widget_t *, sui_canvas_t *);
    void (*measure)   (sui_widget_t *);
    void (*on_event)  (sui_widget_t *, const sui_event_t *);
    void (*destroy)   (sui_widget_t *);
};
```

### 7.3 通用 API

```c
/* 创建 / 销毁 */
sui_widget_t *sui_widget_create(const struct sui_widget_vtable *vt,
                                sui_widget_t *parent);

void sui_widget_destroy(sui_widget_t *w);
void sui_widget_destroy_recursive(sui_widget_t *w);

/* 树操作 */
void sui_widget_add_child(sui_widget_t *parent, sui_widget_t *child);
void sui_widget_remove_child(sui_widget_t *parent, sui_widget_t *child);
void sui_widget_set_parent(sui_widget_t *w, sui_widget_t *new_parent);

/* 几何 */
void sui_widget_set_rect(sui_widget_t *w, int x, int y, int w_, int h_);
void sui_widget_set_pos(sui_widget_t *w, int x, int y);
void sui_widget_set_size(sui_widget_t *w, int w_, int h_);

/* 可见性 / 启用 */
void sui_widget_set_visible(sui_widget_t *w, bool v);
void sui_widget_set_enabled(sui_widget_t *w, bool e);

/* 焦点 */
void sui_widget_focus(sui_widget_t *w);
void sui_widget_blur(sui_widget_t *w);
sui_widget_t *sui_widget_focused(void);

/* 重绘 */
void sui_widget_invalidate(sui_widget_t *w);
void sui_widget_invalidate_rect(sui_widget_t *w, int x, int y, int w_, int h_);

/* 命中测试 */
sui_widget_t *sui_widget_hit_test(sui_widget_t *root, int x, int y);

/* 坐标转换 */
void sui_widget_to_screen(sui_widget_t *w, int *x, int *y);
void sui_widget_from_screen(sui_widget_t *w, int *x, int *y);
```

### 7.4 绘制流程

```c
/* 单帧绘制流程 */
void sui_render(sui_widget_t *root, sui_canvas_t *canvas){
    if (!root->visible) return;

    /* 1. 应用透明度 */
    sui_canvas_push_alpha(canvas, root->opacity);

    /* 2. 绘制背景（材质、圆角、阴影） */
    if (root->bg_color || root->material != SUI_MATERIAL_SOLID) {
        sui_canvas_draw_background(canvas, root,
                                   root->corner_radius,
                                   root->bg_color,
                                   root->shadow);
    }

    /* 3. 调用虚函数绘制自身 */
    if (root->vtable->draw)
        root->vtable->draw(root, canvas);

    /* 4. 绘制焦点环 */
    if (root->focused && root->focusable)
        sui_canvas_draw_focus_ring(canvas, root);

    /* 5. 递归绘制子控件 */
    for (sui_widget_t *c = root->first_child; c; c = c->next_sibling)
        sui_render(c, canvas);

    /* 6. 恢复透明度 */
    sui_canvas_pop_alpha(canvas);
}
```

### 7.5 事件分发流程

```c
void sui_dispatch_event(sui_widget_t *root, const sui_event_t *ev){
    /* 1. 鼠标事件：命中测试 */
    if (ev->type >= SUI_EVENT_MOUSE_MOVE && ev->type <= SUI_EVENT_MOUSE_WHEEL) {
        sui_widget_t *target = sui_widget_hit_test(root, ev->mouse.x, ev->mouse.y);
        if (target) {
            /* 更新 hover 状态 */
            sui_widget_t *old = sui_widget_hovered();
            if (old != target) {
                if (old) sui__set_hovered(old, false);
                sui__set_hovered(target, true);
            }
            /* 从 target 向上冒泡 */
            for (sui_widget_t *w = target; w; w = w->parent) {
                if (w->vtable->on_event) w->vtable->on_event(w, ev);
                if (ev->stopped) break;
            }
        }
    }

    /* 2. 键盘事件：发给焦点控件 */
    if (ev->type == SUI_EVENT_KEY_DOWN || ev->type == SUI_EVENT_KEY_UP) {
        sui_widget_t *f = sui_widget_focused();
        if (f) {
            for (sui_widget_t *w = f; w; w = w->parent) {
                if (w->vtable->on_event) w->vtable->on_event(w, ev);
                if (ev->stopped) break;
            }
        }
    }
}
```

---

## 8. 基础绘制原语

`libsui` 在 `libsuki_gui` 之上扩展以下原语。所有原语操作 `sui_canvas_t`，最终由 `sui_canvas_flush` 提交。

### 8.1 Canvas 结构体

```c
typedef struct {
    sui_window_t *win;          /* 底层窗口 */
    uint32_t     *pixels;       /* 离屏缓冲 */
    int           width, height;
    int           clip_x, clip_y, clip_w, clip_h;
    float         alpha;        /* 全局透明度 */
    int           alpha_stack[16];
    int           alpha_sp;
} sui_canvas_t;

sui_canvas_t *sui_canvas_create(sui_window_t *win);
void sui_canvas_destroy(sui_canvas_t *c);
void sui_canvas_flush(sui_canvas_t *c, int x, int y, int w, int h);
```

### 8.2 原语列表

```c
/* 基础 */
void sui_canvas_set_pixel(sui_canvas_t *c, int x, int y, uint32_t color);
void sui_canvas_fill_rect(sui_canvas_t *c, int x, int y, int w, int h, uint32_t color);
void sui_canvas_draw_line(sui_canvas_t *c, int x1, int y1, int x2, int y2,
                          uint32_t color, int thickness);

/* 圆角 */
void sui_canvas_fill_rounded_rect(sui_canvas_t *c, int x, int y, int w, int h,
                                  int radius, uint32_t color);
void sui_canvas_stroke_rounded_rect(sui_canvas_t *c, int x, int y, int w, int h,
                                    int radius, int thickness, uint32_t color);

/* 圆 / 椭圆 */
void sui_canvas_fill_circle(sui_canvas_t *c, int cx, int cy, int r, uint32_t color);
void sui_canvas_stroke_circle(sui_canvas_t *c, int cx, int cy, int r,
                              int thickness, uint32_t color);

/* 阴影 */
void sui_canvas_draw_shadow(sui_canvas_t *c, int x, int y, int w, int h,
                            int radius, const sui_shadow_t *shadow);

/* 文本 */
void sui_canvas_draw_text(sui_canvas_t *c, int x, int y, const char *text,
                          int font_size, int weight, uint32_t color);
void sui_canvas_draw_text_clipped(sui_canvas_t *c, int x, int y, int w, int h,
                                  const char *text, int font_size, int weight,
                                  uint32_t color, sui_text_overflow_t overflow);
int  sui_canvas_measure_text(sui_canvas_t *c, const char *text,
                             int font_size, int weight);

/* 图像 */
void sui_canvas_draw_bitmap(sui_canvas_t *c, int x, int y, const sui_bitmap_t *bmp);
void sui_canvas_draw_bitmap_scaled(sui_canvas_t *c, int x, int y, int w, int h,
                                   const sui_bitmap_t *bmp);

/* 裁剪 */
void sui_canvas_push_clip(sui_canvas_t *c, int x, int y, int w, int h);
void sui_canvas_pop_clip(sui_canvas_t *c);

/* 透明度 */
void sui_canvas_push_alpha(sui_canvas_t *c, float alpha);
void sui_canvas_pop_alpha(sui_canvas_t *c);

/* 组合：绘制控件背景 */
void sui_canvas_draw_background(sui_canvas_t *c, const sui_widget_t *w,
                                int radius, uint32_t bg,
                                const sui_shadow_t *shadow);

/* 焦点环 */
void sui_canvas_draw_focus_ring(sui_canvas_t *c, const sui_widget_t *w);
```

### 8.3 圆角矩形算法

使用**扫描线 + 四角圆**方式：

```c
void sui_canvas_fill_rounded_rect(sui_canvas_t *c, int x, int y, int w, int h,
                                  int r, uint32_t color){
    if (r <= 0) { sui_canvas_fill_rect(c, x, y, w, h, color); return; }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    /* 中间矩形 */
    sui_canvas_fill_rect(c, x, y + r, w, h - 2 * r, color);

    /* 上下两条 */
    sui_canvas_fill_rect(c, x + r, y, w - 2 * r, r, color);
    sui_canvas_fill_rect(c, x + r, y + h - r, w - 2 * r, r, color);

    /* 四角 */
    sui__fill_corner(c, x + r,     y + r,     r, color, 0);
    sui__fill_corner(c, x + w - r, y + r,     r, color, 1);
    sui__fill_corner(c, x + r,     y + h - r, r, color, 2);
    sui__fill_corner(c, x + w - r, y + h - r, r, color, 3);
}
```

角绘制用中点圆算法，只填充朝向矩形内部的 1/4。

### 8.4 文本渲染

文本必须通过 `fontsrv` 请求字形：

```c
typedef struct {
    int      width, height;
    int      bearing_x, bearing_y;
    int      advance;
    uint8_t *bitmap;  /* 8-bit alpha */
} sui_glyph_t;

/* 从 fontsrv 获取字形（带 LRU 缓存） */
const sui_glyph_t *sui_font_glyph(uint32_t codepoint, int size, int weight);

/* 文本绘制 */
void sui_canvas_draw_text(sui_canvas_t *c, int x, int y, const char *text,
                          int size, int weight, uint32_t color){
    int pen_x = x;
    const uint8_t *p = (const uint8_t *)text;
    while (*p) {
        uint32_t cp = utf8_decode(&p);
        const sui_glyph_t *g = sui_font_glyph(cp, size, weight);
        if (!g) continue;
        /* 混合字形 */
        sui__blit_glyph(c, pen_x + g->bearing_x, y + g->bearing_y, g, color);
        pen_x += g->advance;
    }
}
```

**字形缓存**：LRU，容量 1024，键为 `(codepoint << 16) | (size << 4) | weight`。

### 8.5 文本溢出策略

```c
typedef enum {
    SUI_TEXT_OVERFLOW_CLIP,     /* 直接裁剪 */
    SUI_TEXT_OVERFLOW_ELLIPSIS, /* 结尾省略号 */
    SUI_TEXT_OVERFLOW_WRAP,     /* 换行 */
    SUI_TEXT_OVERFLOW_SHRINK    /* 缩小字号 */
} sui_text_overflow_t;
```

---

## 9. 组件规范

以下每个组件必须实现：结构体、`create`、`destroy`、`draw`、`measure`、`on_event`、公开 setter/getter。

### 9.1 通用状态

所有可交互控件必须支持以下状态：

| 状态 | 视觉变化 |
|---|---|
| Normal | 主题默认 |
| Hover | 背景 +4% 亮度（浅色）/ +6%（深色） |
| Pressed | 背景 -8% 亮度，缩放 0.98 |
| Focused | 2px 强调色焦点环，外扩 2px |
| Disabled | 整体 opacity 0.38，不响应事件 |
| Selected | 强调色 16% 背景，文字强调色 |

### 9.2 Button（按钮）

```c
typedef enum {
    SUI_BTN_PRIMARY,    /* 填充强调色 */
    SUI_BTN_SECONDARY,  /* 透明底 + 描边 */
    SUI_BTN_DANGER,     /* 填充红色 */
    SUI_BTN_GHOST,      /* 无底，强调色文字 */
    SUI_BTN_ICON        /* 正方形，仅图标 */
} sui_button_variant_t;

typedef enum {
    SUI_BTN_SIZE_SM = 26,
    SUI_BTN_SIZE_MD = 32,
    SUI_BTN_SIZE_LG = 38
} sui_button_size_t;

typedef struct {
    sui_widget_t         base;
    sui_button_variant_t variant;
    sui_button_size_t    size;
    char                 text[64];
    const sui_bitmap_t  *icon;
    int                  icon_size;
    void               (*on_click)(sui_widget_t *, void *);
    void                *user;
} sui_button_t;

sui_button_t *sui_button_create(sui_widget_t *parent,
                                sui_button_variant_t v,
                                sui_button_size_t s,
                                const char *text);
void sui_button_set_text(sui_button_t *b, const char *text);
void sui_button_set_icon(sui_button_t *b, const sui_bitmap_t *icon, int size);
void sui_button_on_click(sui_button_t *b, void (*fn)(sui_widget_t *, void *), void *user);
```

**尺寸**：

| 尺寸 | 高度 | 左右内边距 | 字号 | 圆角 |
|---|---|---|---|---|
| SM | 26 | 10 | 12 | 6 |
| MD | 32 | 14 | 13 | 6 |
| LG | 38 | 18 | 14 | 6 |

**绘制规则**：

- PRIMARY：填充 `accent`，文字白，无描边。
- SECONDARY：填充 `surface`，描边 `stroke` 1px，文字 `text_primary`。
- DANGER：填充 `danger`，文字白。
- GHOST：无填充，文字 `accent`，hover 时填充 `accent_soft`。
- ICON：`width == height`，居中图标。

**状态色**：

| 状态 | PRIMARY | SECONDARY |
|---|---|---|
| Normal | accent | surface |
| Hover | accent_hover | surface_hover |
| Pressed | accent_pressed | surface_active |
| Focused | + 2px 焦点环 | + 2px 焦点环 |
| Disabled | opacity 0.38 | opacity 0.38 |

### 9.3 Input（单行输入框）

```c
typedef struct {
    sui_widget_t base;
    char         text[256];
    int          cursor;
    int          sel_start, sel_end;
    char         placeholder[128];
    bool         password;
    bool         readonly;
    const sui_bitmap_t *icon_left;
    void       (*on_change)(sui_widget_t *, const char *, void *);
    void       (*on_submit)(sui_widget_t *, const char *, void *);
    void        *user;
} sui_input_t;

sui_input_t *sui_input_create(sui_widget_t *parent, const char *placeholder);
void sui_input_set_text(sui_input_t *i, const char *text);
const char *sui_input_text(const sui_input_t *i);
void sui_input_set_password(sui_input_t *i, bool pw);
void sui_input_set_icon(sui_input_t *i, const sui_bitmap_t *icon);
```

**尺寸**：高度 32，左右内边距 11，圆角 6，字号 13。

**颜色**：

| 元素 | 颜色 |
|---|---|
| 背景 | `input_bg` |
| 背景 hover | `surface_hover` |
| 背景 focus | `surface_hover` |
| 描边 | `stroke` |
| 描边 focus | `accent` + 3px `accent_soft` 外阴影 |
| 文字 | `text_primary` |
| Placeholder | `text_tertiary` |
| 光标 | `accent`，1px 宽，闪烁 530ms |
| 选区 | `accent_soft` |

**带图标时**：左内边距改为 31，图标在 x=10，尺寸 14。

**键盘**：

- 左右方向键移动光标。
- Shift + 方向键选择。
- Home/End 跳转。
- Ctrl+A 全选。
- Ctrl+C/V/X 复制/粘贴/剪切（通过系统剪贴板 IPC）。
- Enter 触发 `on_submit`。

### 9.4 TextArea（多行）

同 `sui_input_t`，但：

- 默认高度 76，最小 60。
- 支持自动换行。
- 支持垂直滚动。
- Enter 插入换行，Ctrl+Enter 提交。

### 9.5 Checkbox（复选框）

```c
typedef struct {
    sui_widget_t base;
    char         label[128];
    bool         checked;
    bool         indeterminate;  /* 三态 */
    void       (*on_change)(sui_widget_t *, bool, void *);
    void        *user;
} sui_checkbox_t;

sui_checkbox_t *sui_checkbox_create(sui_widget_t *parent, const char *label, bool checked);
```

**尺寸**：方框 18×18，圆角 4，标签与方框间距 9，字号 13。

**状态**：

| 状态 | 方框 | 勾 |
|---|---|---|
| Unchecked | 描边 `box_border` 1.5px，透明底 | 无 |
| Checked | 填充 `accent`，描边 `accent` | 白色 SVG 勾 |
| Indeterminate | 填充 `accent` | 白色横线 |
| Hover | 描边 `text_tertiary` | — |
| Focused | + 3px `accent_soft` 外环 | — |
| Disabled | opacity 0.38 | — |

勾的 SVG 路径：`M2.5 6.2l2.3 2.3 4.7-5`（viewBox 12×12，stroke-width 2，round cap/join）。

### 9.6 Radio（单选）

同 Checkbox，但：

- 方框为 18×18 圆形。
- 选中时内部 8×8 圆点填充 `accent`。
- 需与同组其他 Radio 互斥（通过 `group_id` 或父容器自动分组）。

### 9.7 Switch（开关）

```c
typedef struct {
    sui_widget_t base;
    bool         on;
    void       (*on_change)(sui_widget_t *, bool, void *);
    void        *user;
} sui_switch_t;
```

**尺寸**：40×20，圆角 9999，滑块 16，内边距 2。

**动画**：

- 滑块位移：`transform: translateX(0 / 20)`，220ms `SUI_EASE_STANDARD`。
- 背景色过渡：220ms。
- 按下时滑块宽度临时变 19px（拉伸效果）。

**颜色**：

| 状态 | 轨道 | 滑块 |
|---|---|---|
| Off | `switch_off` | 白 |
| On | `accent` | 白 |
| Disabled | opacity 0.38 | — |

### 9.8 Slider（滑块）

```c
typedef struct {
    sui_widget_t base;
    int          min, max, value;
    int          step;
    bool         show_value;
    const char  *unit;  /* "%", "px" */
    void       (*on_change)(sui_widget_t *, int, void *);
    void        *user;
} sui_slider_t;
```

**尺寸**：轨道高 4，圆角 9999，滑块 16 圆。

**颜色**：

| 元素 | 颜色 |
|---|---|
| 轨道 | `range_track` |
| 已填充部分 | `accent` |
| 滑块 | `range_thumb`，阴影 `0 1 4 rgba(0,0,0,0.4)` |
| 滑块 hover | 缩放 1.1 |
| 滑块 active | 缩放 1.18 |

**交互**：点击轨道任意位置跳转，拖动实时更新。

### 9.9 Select（下拉选择）

```c
typedef struct {
    sui_widget_t base;
    char         options[16][64];
    int          option_count;
    int          selected;
    bool         open;
    void       (*on_change)(sui_widget_t *, int, void *);
    void        *user;
} sui_select_t;
```

**按钮**：同 Input 尺寸（高 32，圆角 6），右侧有 12×12 下拉箭头（`caret`）。

**菜单**：绝对定位在按钮下方 +5px，宽同按钮，材质 Acrylic，圆角 8，阴影 `SUI_SHADOW_MENU`。

**菜单项**：高 28，圆角 5，字号 12.5。选中项显示 12×12 勾，颜色 `accent`。

**动画**：菜单淡入 + 上移 6px，140ms `SUI_EASE_DECEL`。

**键盘**：上下方向键切换，Enter 确认，Esc 关闭。

### 9.10 Segmented（分段控件）

```c
typedef struct {
    sui_widget_t base;
    char         segments[8][32];
    int          count;
    int          selected;
    void       (*on_change)(sui_widget_t *, int, void *);
    void        *user;
} sui_segmented_t;
```

**尺寸**：外高 30（含 2px 内边距），段高 26，段圆角 5，外圆角 7。

**颜色**：

| 元素 | 颜色 |
|---|---|
| 外底 | `surface` |
| 段 Normal | 透明，文字 `text_secondary` |
| 段 Active | 浅色模式 `#FFFFFF`，深色模式 `text_primary` 12% 叠加 |
| 段 Active 阴影 | `0 1 3 rgba(0,0,0,0.22)` |

**切换动画**：高亮块平滑位移（200ms `SUI_EASE_STANDARD`）。

### 9.11 Tabs（标签页）

```c
typedef struct {
    sui_widget_t base;
    char         tabs[8][32];
    int          count;
    int          selected;
    sui_widget_t *panels[8];  /* 对应的内容面板 */
    void       (*on_change)(sui_widget_t *, int, void *);
    void        *user;
} sui_tabs_t;
```

**尺寸**：标签高 34，左右内边距 12，字号 13。

**颜色**：

| 元素 | 颜色 |
|---|---|
| 底部分隔线 | `stroke_soft` |
| 标签 Normal | `text_secondary` |
| 标签 Active | `text_primary`，字重 500 |
| 激活下划线 | `accent`，2px，左右缩进 8 |

**切换动画**：下划线平滑位移（200ms），面板淡入 + 上移 8px（220ms `SUI_EASE_DECEL`）。

### 9.12 List（列表）

```c
typedef struct {
    sui_widget_t base;
    sui_list_item_t *items;
    int              count;
    int              selected;
    bool             multi_select;
    void           (*on_select)(sui_widget_t *, int, void *);
    void            *user;
} sui_list_t;

typedef struct {
    const sui_bitmap_t *avatar;    /* 或 NULL */
    uint32_t            avatar_bg; /* 渐变起始色 */
    char                avatar_text[4];
    char                title[64];
    char                subtitle[128];
    char                badge[16];
    uint32_t            badge_color;
} sui_list_item_t;
```

**列表项**：高 50（含 9px 上下内边距），圆角 7，字号 13/11.5。

**颜色**：

| 状态 | 背景 |
|---|---|
| Normal | 透明 |
| Hover | `surface_hover` |
| Selected | `accent_soft` |

**头像**：32×32 圆，渐变背景，白色居中文字（1-2 字符），字号 12.5 字重 600。

**徽标**：高 18，内边距 2×7，圆角 9999，字号 10.5 字重 600。

| 类型 | 背景 | 文字 |
|---|---|---|
| Accent | `accent_soft` | `accent` |
| Gray | `surface` | `text_secondary` |
| Green | success 18% | success |

### 9.13 Card（卡片）

```c
typedef struct {
    sui_widget_t base;
    char         title[64];
    char         body[256];
    const sui_bitmap_t *icon;
} sui_card_t;
```

**尺寸**：内边距 14，圆角 8，标题字号 13.5 字重 600，正文字号 12。

**颜色**：

| 元素 | 颜色 |
|---|---|
| 背景 | `surface` |
| 背景 hover | `surface_hover` |
| 描边 | `stroke_soft` |
| 描边 hover | `stroke` |
| 标题 | `text_primary` |
| 正文 | `text_secondary` |

**hover 动画**：`translateY(-1px)`，160ms。

### 9.14 Progress（进度条）

```c
typedef struct {
    sui_widget_t base;
    int          value;      /* 0 - 100 */
    int          thickness;  /* 默认 5 */
    uint32_t     color;      /* 0 = accent */
    bool         indeterminate;
} sui_progress_t;
```

**尺寸**：默认高 5，圆角 9999。

**动画**：值变化时宽度过渡 500ms `SUI_EASE_STANDARD`。

**Indeterminate**：循环扫描动画，周期 1.4s。

### 9.15 Alert（提示条）

```c
typedef enum {
    SUI_ALERT_INFO,
    SUI_ALERT_WARN,
    SUI_ALERT_SUCCESS,
    SUI_ALERT_ERROR
} sui_alert_type_t;

typedef struct {
    sui_widget_t   base;
    sui_alert_type_t type;
    char           title[64];
    char           body[256];
} sui_alert_t;
```

**尺寸**：内边距 11×13，圆角 8，标题字号 12.5 字重 600，正文字号 12.5。

**颜色**：

| 类型 | 背景 | 描边 | 文字 |
|---|---|---|---|
| Info | accent 12% | accent 28% | accent 提亮 |
| Warn | warning 12% | warning 28% | warning 提亮 |
| Success | success 12% | success 28% | success 提亮 |
| Error | danger 12% | danger 28% | danger 提亮 |

标题用 `text_primary`。

### 9.16 Menu（菜单）

```c
typedef struct {
    sui_widget_t  base;
    sui_menu_item_t *items;
    int             count;
} sui_menu_t;

typedef struct {
    char     label[64];
    char     shortcut[16];
    bool     separator;
    bool     danger;
    bool     disabled;
    const sui_bitmap_t *icon;
    void   (*on_click)(sui_widget_t *, void *);
    void    *user;
} sui_menu_item_t;
```

**容器**：宽 216，内边距 5，圆角 8，材质 Acrylic，阴影 `SUI_SHADOW_MENU`。

**菜单项**：高 28，圆角 5，字号 12.5，左右内边距 9。

**快捷键**：右对齐，字号 11，颜色 `text_tertiary`。

**分隔线**：高 1，颜色 `stroke_soft`，上下外边距 5，左右缩进 7。

**危险项**：文字 `danger`，hover 背景 `danger 14%`。

**动画**：淡入 + 上移 6px，140ms。

### 9.17 Toast（通知）

```c
typedef enum {
    SUI_TOAST_INFO,
    SUI_TOAST_SUCCESS,
    SUI_TOAST_WARN,
    SUI_TOAST_ERROR
} sui_toast_type_t;

typedef struct {
    sui_toast_type_t type;
    char             title[64];
    char             body[256];
    int              duration_ms;  /* 默认 4000 */
} sui_toast_desc_t;

void sui_toast_show(const sui_toast_desc_t *desc);
```

**容器**：位置右上角（`top: 18, right: 18`），多个垂直堆叠，间距 10。

**尺寸**：宽最大 330，内边距 11×14，圆角 10。

**颜色**：背景 `toast_bg`，描边 `stroke`，阴影 `SUI_SHADOW_MENU`。

**图标**：30×30 渐变圆，白色字符（`i` / `✓` / `!` / `✕`）。

**动画**：

- 进入：`translateX(28px) scale(0.96)` → `translateX(0) scale(1)`，300ms `SUI_EASE_DECEL`。
- 停留：`duration_ms`。
- 退出：反向，280ms。

### 9.18 Dialog（对话框）

```c
typedef struct {
    char         title[64];
    char         body[256];
    const char  *icon_svg;
    sui_dialog_action_t actions[4];
    int          action_count;
} sui_dialog_desc_t;

typedef struct {
    char      text[32];
    uint32_t  style;  /* PRIMARY / SECONDARY / DANGER */
    void    (*on_click)(void *);
    void     *user;
} sui_dialog_action_t;

void sui_dialog_show(const sui_dialog_desc_t *desc);
void sui_dialog_close(void);
```

**遮罩**：全屏，`rgba(0,0,0,0.35)`，模糊 6px。

**对话框**：宽最大 400，内边距 22×22×18，圆角 12，材质 Mica，阴影 `SUI_SHADOW_DIALOG`。

**图标**：44×44 圆，`accent_soft` 背景，`accent` 图标，居中，下边距 14。

**标题**：字号 15 字重 600，居中，下边距 7。

**正文**：字号 12.5，`text_secondary`，居中，行高 1.6，下边距 20。

**按钮组**：右对齐，间距 8，每个按钮 `flex: 1`。

**动画**：遮罩淡入 220ms，对话框 `scale(0.94) → scale(1)` + 淡入 260ms `SUI_EASE_STANDARD`。

**键盘**：Esc 关闭，Enter 触发主按钮。

### 9.19 Tooltip（工具提示）

```c
void sui_tooltip_show(sui_widget_t *anchor, const char *text);
void sui_tooltip_hide(void);
```

**尺寸**：内边距 6×10，圆角 6，字号 12，材质 Smoke。

**位置**：默认在锚点下方 +6px，超出屏幕时翻转。

**动画**：淡入 + 上移 4px，120ms。悬停 500ms 后显示。

### 9.20 Badge（徽标）

```c
typedef struct {
    sui_widget_t base;
    char         text[8];
    uint32_t     color;
} sui_badge_t;
```

**尺寸**：高 18，内边距 2×7，圆角 9999，字号 10.5 字重 600。

### 9.21 Avatar（头像）

```c
typedef struct {
    sui_widget_t base;
    int          size;       /* 默认 32 */
    uint32_t     gradient_a;
    uint32_t     gradient_b;
    char         text[4];
    const sui_bitmap_t *image;
} sui_avatar_t;
```

**尺寸**：默认 32×32，圆角 9999。

**渐变**：45°，从 `gradient_a` 到 `gradient_b`。

**文字**：白色居中，字号 = `size * 0.39`，字重 600。

---

## 10. 交互与状态机

### 10.1 指针状态

```c
typedef enum {
    SUI_POINTER_NORMAL = 0,
    SUI_POINTER_HOVER,
    SUI_POINTER_POINTER,  /* 手型 */
    SUI_POINTER_TEXT,     /* 文本 I 型 */
    SUI_POINTER_RESIZE_H,
    SUI_POINTER_RESIZE_V,
    SUI_POINTER_MOVE,
    SUI_POINTER_WAIT,
    SUI_POINTER_DISABLED
} sui_pointer_t;

void sui_set_pointer(sui_pointer_t p);
```

### 10.2 悬停延迟

- 悬停高亮：立即（0ms）。
- Tooltip：延迟 500ms 显示，100ms 隐藏。

### 10.3 点击判定

- 按下与释放必须落在同一控件内才触发 `on_click`。
- 按下后移出控件范围，取消点击。

### 10.4 焦点链

- Tab 键在 `focusable` 控件之间循环。
- Shift+Tab 反向循环。
- 焦点顺序为**树的前序遍历**。
- 窗口失焦时保存焦点控件，重新获焦时恢复。

### 10.5 键盘快捷键

| 组合 | 行为 |
|---|---|
| Tab / Shift+Tab | 焦点切换 |
| Enter / Space | 激活当前焦点控件 |
| Esc | 关闭当前浮层 / 取消 |
| 方向键 | 在列表、菜单、分段中导航 |
| Home / End | 跳到首 / 尾 |
| Ctrl+A | 全选 |
| Ctrl+C / V / X | 复制 / 粘贴 / 剪切 |
| Ctrl+Z / Y | 撤销 / 重做 |
| Alt+F4 | 关闭窗口 |

### 10.6 拖拽

```c
typedef enum {
    SUI_DRAG_NONE = 0,
    SUI_DRAG_WINDOW,      /* 拖动标题栏 */
    SUI_DRAG_RESIZE,      /* 拖拽边缘 */
    SUI_DRAG_SLIDER,      /* 滑块 */
    SUI_DRAG_SELECT,      /* 文本选择 */
    SUI_DRAG_ITEM         /* 列表项拖拽 */
} sui_drag_mode_t;
```

拖拽阈值：移动超过 4px 才进入拖拽状态。

---

## 11. 动效规范

### 11.1 时长矩阵

| 场景 | 时长 | 缓动 |
|---|---|---|
| 悬停背景变化 | 100–120ms | STANDARD |
| 按下缩放 | 80ms | ACCEL |
| 焦点环出现 | 120ms | DECEL |
| 开关切换 | 220ms | STANDARD |
| 分段高亮位移 | 200ms | STANDARD |
| 标签页下划线 | 200ms | STANDARD |
| 下拉菜单展开 | 140ms | DECEL |
| 下拉菜单收起 | 120ms | ACCEL |
| Toast 进入 | 300ms | DECEL |
| Toast 退出 | 280ms | ACCEL |
| 对话框进入 | 260ms | STANDARD |
| 对话框退出 | 200ms | ACCEL |
| 窗口打开 | 500ms | SPRING |
| 窗口关闭 | 320ms | ACCEL |
| 窗口最小化 | 380ms | STANDARD |
| 页面切换 | 280ms | DECEL |
| 卡片 hover | 160ms | STANDARD |
| 进度条值变化 | 500ms | STANDARD |

### 11.2 可打断原则

- 任何动效都必须在**新状态到来时立即重定向**，不排队。
- 例如开关快速连点，滑块应从中途位置平滑过渡到新目标，不回到起点。
- 实现方式：记录 `current_value`、`target_value`、`start_time`、`duration`，每帧计算 `t`。

### 11.3 动画驱动器

```c
typedef struct {
    float  from;
    float  to;
    float  current;
    uint64_t start_ms;
    int    duration_ms;
    sui_easing_t easing;
    bool   running;
} sui_anim_t;

void sui_anim_start(sui_anim_t *a, float from, float to,
                    int duration_ms, sui_easing_t e);
void sui_anim_tick(sui_anim_t *a, uint64_t now_ms);
bool sui_anim_running(const sui_anim_t *a);
```

主循环每帧调用 `sui_anim_tick`，仅当有动画运行时触发重绘。

### 11.4 减少动效

当 `sui_theme()->reduce_motion == true`：

- 所有 `duration > 120ms` 的动画改为 120ms 纯淡入淡出。
- 禁用弹簧、缩放、位移。
- 保留透明度过渡。

```c
void sui_theme_set_reduce_motion(bool reduce);
```

---

## 12. 窗口与桌面规范

### 12.1 窗口

**尺寸**：

- 标题栏高 38。
- 圆角 12。
- 最小宽 320，最小高 200。

**标题栏布局**：

- 左侧：交通灯，3 个 12px 圆点，间距 8。
- 中间：标题，字号 13 字重 600，绝对居中。
- 右侧：工具栏按钮，每个 28×24，间距 2。

**交通灯**：

| 按钮 | 颜色 | Hover 符号 |
|---|---|---|
| Close | `#FF5F57` | ✕（黑色 55%） |
| Min | `#FEBC2E` | − |
| Max | `#28C840` | + |

**窗口阴影**：活动 `SUI_SHADOW_WINDOW_ACTIVE`，非活动 ×0.5。

**非活动窗口**：标题文字 `text_secondary`，交通灯去饱和 30%。

### 12.2 桌面

**顶部菜单栏**：

- 高 24。
- 左侧：当前应用名、菜单项。
- 右侧：状态图标（Wi-Fi、电池、音量）、时钟。
- 材质 Acrylic，透明度 0.75。

**底部任务栏**：

- 高 48。
- 居中图标：48×48 应用图标，间距 6。
- 运行指示：底部 3×3 圆点，颜色 `accent`。
- 左侧：开始按钮。
- 右侧：托盘、时钟。

**Dock 模式**（备选）：

- 图标 52×52，间距 8。
- 悬停放大 1.4 倍，相邻图标 1.2 倍。
- 运行指示：底部 4×4 圆点。

### 12.3 Snap 布局

拖拽窗口到屏幕边缘触发：

| 区域 | 行为 |
|---|---|
| 左边缘 | 左半屏 |
| 右边缘 | 右半屏 |
| 顶部 | 最大化 |
| 左上角 | 左上 1/4 |
| 右上角 | 右上 1/4 |
| 左下角 | 左下 1/4 |
| 右下角 | 右下 1/4 |

触发阈值：距离边缘 8px，悬停 300ms 后预览。

### 12.4 壁纸与 Mica

- 壁纸加载后，取其平均色作为 `mica_base`。
- 算法：降采样至 8×8，计算加权平均，转 HSL，饱和度 ×0.6，亮度调整至浅色 0.95 / 深色 0.11。

---

## 13. 事件系统

### 13.1 事件类型

```c
typedef enum {
    SUI_EVENT_NONE = 0,

    /* 鼠标 */
    SUI_EVENT_MOUSE_MOVE,
    SUI_EVENT_MOUSE_DOWN,
    SUI_EVENT_MOUSE_UP,
    SUI_EVENT_MOUSE_WHEEL,
    SUI_EVENT_MOUSE_ENTER,
    SUI_EVENT_MOUSE_LEAVE,

    /* 键盘 */
    SUI_EVENT_KEY_DOWN,
    SUI_EVENT_KEY_UP,
    SUI_EVENT_KEY_CHAR,

    /* 焦点 */
    SUI_EVENT_FOCUS_IN,
    SUI_EVENT_FOCUS_OUT,

    /* 窗口 */
    SUI_EVENT_WINDOW_CLOSE,
    SUI_EVENT_WINDOW_RESIZE,
    SUI_EVENT_WINDOW_MOVE,
    SUI_EVENT_WINDOW_FOCUS,
    SUI_EVENT_WINDOW_BLUR,
    SUI_EVENT_WINDOW_MINIMIZE,
    SUI_EVENT_WINDOW_RESTORE,

    /* 定时器 */
    SUI_EVENT_TIMER,
    SUI_EVENT_ANIM_TICK,

    /* 主题 */
    SUI_EVENT_THEME_CHANGED
} sui_event_type_t;

typedef struct {
    sui_event_type_t type;
    uint64_t         timestamp;
    bool             stopped;   /* 是否已消费 */

    union {
        struct { int x, y; int dx, dy; int button; } mouse;
        struct { uint32_t key; uint32_t mods; char ch; } key;
        struct { int w, h; } size;
        struct { uint64_t id; void *data; } timer;
    };
} sui_event_t;

/* 停止事件冒泡 */
void sui_event_stop(sui_event_t *ev);
```

### 13.2 事件循环

```c
void sui_main_loop(sui_window_t *win){
    sui_event_t ev;
    uint64_t last_frame = sui_now_ms();

    while (sui_is_running()) {
        /* 1. 拉取系统事件 */
        while (suki_poll_event(win, &ev)) {
            sui_event_t e = convert_event(&ev);
            sui_dispatch_event(sui_root(), &e);
        }

        /* 2. 动画 tick */
        uint64_t now = sui_now_ms();
        if (now - last_frame >= 16) {  /* 60 FPS */
            sui_anim_tick_all(now);
            last_frame = now;
        }

        /* 3. 重绘 */
        if (sui_needs_redraw()) {
            sui_canvas_t *c = sui_canvas_create(win);
            sui_render(sui_root(), c);
            sui_canvas_flush(c, 0, 0, c->width, c->height);
            sui_canvas_destroy(c);
        }

        /* 4. 空闲等待 */
        sui_wait_event(win, 16);
    }
}
```

### 13.3 脏矩形管理

```c
typedef struct {
    int x, y, w, h;
} sui_rect_t;

void sui_invalidate_rect(const sui_rect_t *r);
void sui_invalidate_all(void);
bool sui_needs_redraw(void);
const sui_rect_t *sui_dirty_region(int *count);
```

同一帧内多个 `invalidate` 调用合并：

- 若两矩形相邻且合并后面积不增，则合并。
- 最多保留 8 个矩形，超出则合并为全屏。

---

## 14. API 汇总

### 14.1 初始化

```c
/* 库初始化，必须在使用任何 API 前调用 */
int  sui_init(sui_window_t *win);

/* 关闭库 */
void sui_shutdown(void);

/* 主循环 */
void sui_main_loop(void);
void sui_quit(void);
```

### 14.2 根控件

```c
sui_widget_t *sui_root(void);
void sui_set_root(sui_widget_t *root);
```

### 14.3 主题

```c
const sui_theme_t *sui_theme(void);
void sui_theme_set(const sui_theme_t *t);
void sui_theme_set_dark(bool dark);
void sui_theme_set_accent(sui_accent_t preset, uint32_t custom);
void sui_theme_set_reduce_motion(bool reduce);
void sui_theme_on_change(sui_theme_changed_fn fn, void *user);
```

### 14.4 材质

```c
const sui_compositor_caps_t *sui_compositor_caps(void);
void suki_set_window_material(suki_window_t *win, const sui_material_desc_t *desc);
```

### 14.5 控件创建

```c
sui_button_t    *sui_button_create(sui_widget_t *parent, sui_button_variant_t v, sui_button_size_t s, const char *text);
sui_input_t     *sui_input_create(sui_widget_t *parent, const char *placeholder);
sui_textarea_t  *sui_textarea_create(sui_widget_t *parent, const char *placeholder);
sui_checkbox_t  *sui_checkbox_create(sui_widget_t *parent, const char *label, bool checked);
sui_radio_t     *sui_radio_create(sui_widget_t *parent, const char *label, const char *group);
sui_switch_t    *sui_switch_create(sui_widget_t *parent, bool on);
sui_slider_t    *sui_slider_create(sui_widget_t *parent, int min, int max, int value);
sui_select_t    *sui_select_create(sui_widget_t *parent);
sui_segmented_t *sui_segmented_create(sui_widget_t *parent, const char **items, int count);
sui_tabs_t      *sui_tabs_create(sui_widget_t *parent, const char **items, int count);
sui_list_t      *sui_list_create(sui_widget_t *parent);
sui_card_t      *sui_card_create(sui_widget_t *parent, const char *title, const char *body);
sui_progress_t  *sui_progress_create(sui_widget_t *parent, int value);
sui_alert_t     *sui_alert_create(sui_widget_t *parent, sui_alert_type_t type, const char *title, const char *body);
sui_avatar_t    *sui_avatar_create(sui_widget_t *parent, const char *text, int size);
sui_badge_t     *sui_badge_create(sui_widget_t *parent, const char *text, uint32_t color);
```

### 14.6 浮层

```c
void sui_toast_show(const sui_toast_desc_t *desc);
void sui_dialog_show(const sui_dialog_desc_t *desc);
void sui_dialog_close(void);
void sui_menu_show(sui_menu_t *menu, int x, int y);
void sui_menu_hide(void);
void sui_tooltip_show(sui_widget_t *anchor, const char *text);
void sui_tooltip_hide(void);
```

### 14.7 布局

```c
void sui_layout_apply(sui_widget_t *root);
void sui_widget_invalidate_layout(sui_widget_t *w);
void sui_widget_set_layout(sui_widget_t *w, const sui_layout_params_t *params);
```

### 14.8 绘制

```c
sui_canvas_t *sui_canvas_create(sui_window_t *win);
void sui_canvas_destroy(sui_canvas_t *c);
void sui_canvas_flush(sui_canvas_t *c, int x, int y, int w, int h);
```

---

## 15. 文件结构与实现约束

### 15.1 目录结构

```
user/libsui/
├── include/
│   ├── sui.h                 # 总入口，include 以下所有
│   ├── sui_types.h           # 基础类型
│   ├── sui_theme.h           # 主题令牌与 API
│   ├── sui_material.h        # 材质
│   ├── sui_canvas.h          # 绘制原语
│   ├── sui_widget.h          # 控件基类
│   ├── sui_layout.h          # 布局
│   ├── sui_event.h           # 事件
│   ├── sui_anim.h            # 动画
│   ├── sui_button.h
│   ├── sui_input.h
│   ├── sui_checkbox.h
│   ├── sui_switch.h
│   ├── sui_slider.h
│   ├── sui_select.h
│   ├── sui_segmented.h
│   ├── sui_tabs.h
│   ├── sui_list.h
│   ├── sui_card.h
│   ├── sui_progress.h
│   ├── sui_alert.h
│   ├── sui_menu.h
│   ├── sui_toast.h
│   ├── sui_dialog.h
│   └── sui_tooltip.h
├── src/
│   ├── sui.c                 # 初始化与主循环
│   ├── theme.c
│   ├── material.c
│   ├── canvas.c
│   ├── canvas_rounded.c      # 圆角 / 阴影算法
│   ├── font.c                # fontsrv 客户端 + 字形缓存
│   ├── widget.c
│   ├── layout.c
│   ├── event.c
│   ├── anim.c
│   ├── button.c
│   ├── input.c
│   ├── checkbox.c
│   ├── switch.c
│   ├── slider.c
│   ├── select.c
│   ├── segmented.c
│   ├── tabs.c
│   ├── list.c
│   ├── card.c
│   ├── progress.c
│   ├── alert.c
│   ├── menu.c
│   ├── toast.c
│   ├── dialog.c
│   └── tooltip.c
└── Makefile
```

### 15.2 编译约束

- C11 标准。
- 无外部依赖（除 `libsuki_gui`、`fontsrv` 客户端）。
- 每个源文件必须能独立编译。
- 头文件可重复包含。
- 所有公开函数必须有文档注释。

### 15.3 性能约束

| 指标 | 目标 |
|---|---|
| 单帧渲染（1920×1080） | < 8ms |
| 空转 CPU 占用 | < 1% |
| 内存占用（含字形缓存） | < 4 MiB |
| 最大控件数 | 4096 |
| 最大嵌套深度 | 32 |

### 15.4 渲染优化

- **脏矩形**：只重绘变化的区域。
- **图层缓存**：静态控件（如卡片背景）缓存为位图。
- **字形缓存**：LRU 1024 项。
- **圆角缓存**：相同 `(w, h, r)` 的圆角遮罩缓存为 8-bit alpha 位图。
- **阴影缓存**：相同 `(w, h, r, level)` 的阴影缓存。
- **OOL 分片**：`suki_flush` 调用时若区域超过 64 KiB，自动分片。

### 15.5 线程模型

- 主线程：事件循环 + 渲染。
- 工作线程：字体光栅化（通过 `fontsrv` IPC）。
- 所有 UI 操作必须在主线程。

### 15.6 内存管理

- 控件生命周期由父控件管理。
- `sui_widget_destroy` 自动从父控件移除。
- 字符串使用固定大小数组（`char[N]`），禁止动态分配。
- 大位图使用 `sys_mmap` 分配，页对齐。

---

## 16. 附录

### 16.1 快速参考卡

**圆角**：窗口 12 / 面板 10 / 卡片 8 / 控件 6 / 小元素 4 / 胶囊 9999  
**间距**：4 / 8 / 12 / 16 / 24 / 32 / 48  
**字号**：11 / 12 / 13 / 15 / 20 / 24 / 28  
**按钮高**：26 / 32 / 38  
**输入框高**：32  
**开关**：40×20  
**复选框**：18×18  
**滑块**：轨道 4 / 滑块 16  
**头像**：32  
**列表项高**：50  
**标签页高**：34  
**标题栏高**：38  
**任务栏高**：48  

### 16.2 状态颜色速查

| 状态 | 背景 | 文字 | 描边 |
|---|---|---|---|
| Normal | `surface` | `text_primary` | `stroke` |
| Hover | `surface_hover` | `text_primary` | `stroke` |
| Pressed | `surface_active` | `text_primary` | `stroke` |
| Focused | `surface` | `text_primary` | `accent` + 3px `accent_soft` |
| Disabled | `surface` @ 0.38 | `text_disabled` | `stroke` @ 0.38 |
| Selected | `accent_soft` | `accent` | `accent` |

### 16.3 键盘导航顺序

1. 侧边栏
2. 主内容区（按树的前序）
3. 工具栏
4. 状态栏

### 16.4 参考实现

- `step72.md`：窗口 API、OOL 零拷贝
- `step73.md`：WM 焦点 / 拖拽 / 键盘转发
- `step75.md`：display_server 重构为纯合成器
- `step77.md`：拖拽释放修复
- `step53.md`：FreeType 字体服务
- `step86.md`：TTY 环形管道

### 16.5 版本历史

| 版本 | 日期 | 变更 |
|---|---|---|
| v1.0 | 2025-XX-XX | 初版，涵盖所有组件与令牌 |

---