/*
 * user/display_server.c
 * -----------------------------------------------------------------------------
 * Ring3 显示服务（SukiOS 的图形合成器）。
 *
 * 设计（依据用户需求 + osdev_wiki Bochs_VBE_extensions）：
 *   1) 内核在 fb_init() 内已通过 Bochs VBE（BGA，PCI 0x1234:0x1111）IO 端口把
 *      线性帧缓冲设到 display.cfg 声明的 1280x720@32bpp（见 kernel bga.c）。
 *      本服务经 SYS_FRAMEBUFFER_MAP 获得帧缓冲的【用户态】线性映射（fb_user_va）。
 *   2) 内核基本初始化完成后、任何用户态 shell 之前启动本服务（见 kmain.c
 *      boot_late_init：shell 暂不启动）。本服务完成桌面合成、进入消息循环前
 *      调用 SYS_DISPLAY_READY，内核随即关闭 fbcon 对真实屏幕的写——内核诊断
 *      不再覆盖桌面，而是被捕获进内核环形管道（类似 dmesg）。
 *   3) 启动阶段把内核启动日志（SYS_CONSOLE_READ）刷到桌面内的终端窗口；之后
 *      循环接收经 DISPLAY_PORT 转发的文本消息（如后续 shell 的输出），渲染到
 *      同一个终端窗口。
 *
 * 像素格式：xRGB32（0xRRGGBB，每像素 4 字节，alpha 忽略）。
 *
 * 鼠标光标渲染采用【像素快照法】：每个光标位置都先保存其覆盖区域的背景像素，
 * 移动时先恢复旧位置背景再绘制新位置。这样终端文本层与光标层互不破坏——
 * 文本重绘不会擦成字符，光标移动也不会在桌面/文本上留残影。
 * 文本消息与鼠标事件都经 DISPLAY_PORT 投递，二者 msgh_id 分区（文本=1，
 * 鼠标=101/102/103）以避免历史版本把鼠标坐标当字符串打印造成"移动鼠标出现字符"。
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "lib/suki.h"

/* 显示服务消息 id（内核 console.c 的 user_puts 转发文本时须用同一值）。
 * 此处集中定义，避免与内核侧漂移。 */
#define DISP_MSG_TEXT 1

/* 鼠标事件消息 id（与 user/mouse_server.c 中定义保持一致；鼠标驱动经
 * DISPLAY_PORT 把光标事件发给显示服务，由本服务统一渲染）。
 * 必须与 mouse_server.c 完全一致，否则主循环无法识别鼠标事件。 */
#define MOUSE_MSG_MOVE   101
#define MOUSE_MSG_BUTTON 102
#define MOUSE_MSG_WHEEL  103
#define MOUSE_CURSOR_W 12
#define MOUSE_CURSOR_H 18

/*
 * fb_map_result_t —— 必须与内核 include/kernel/framebuffer.h 的 fb_map_result_t
 * **逐字节布局一致**（结构体 ABI 跨特权边界传递，错位会导致 width/height/pitch
 * 读错、用户态越界写帧缓冲触发 #PF）。内核布局（含 uint64_t 对齐填充）：
 *   offset 0  : uint32_t enabled
 *   offset 4  : (pad 4)
 *   offset 8  : uint64_t fb_user_va
 *   offset 16 : uint64_t fb_phys
 *   offset 24 : uint32_t pitch
 *   offset 28 : uint32_t width
 *   offset 32 : uint32_t height
 *   offset 36 : uint32_t bpp
 *   offset 40 : uint32_t cfg_width
 *   offset 44 : uint32_t cfg_height
 */
typedef struct fb_map_result {
    uint32_t enabled;
    uint32_t _pad0;          /* 与内核对齐填充保持一致 */
    uint64_t fb_user_va;
    uint64_t fb_phys;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t cfg_width;
    uint32_t cfg_height;
} fb_map_result_t;

/* ---- 字体：8x8 内置点阵（公有领域 font8x8，ASCII 0x20-0x7E） ----
 * display_server 是用户态服务，不能链接内核的 font8x8.c（内核侧由 font.h 以
 * extern 声明 font8x8_basic 暴露，用户态链接不到），故此处内嵌一份副本，
 * 命名 disp_font8x8 以与内核 extern 声明隔离。与 kernel/arch/x86_64/font8x8.c
 * 内容一致（单一数据源以该文件为权威）。C99 designated initializer，未指定的
 * 控制字符项自动为零（视为空字形）。 */
static const uint8_t disp_font8x8[128][8] = {
    [0x20] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* space */
    [0x21] = {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* ! */
    [0x22] = {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, /* " */
    [0x23] = {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, /* # */
    [0x24] = {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, /* $ */
    [0x25] = {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, /* % */
    [0x26] = {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, /* & */
    [0x27] = {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, /* ' */
    [0x28] = {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, /* ( */
    [0x29] = {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, /* ) */
    [0x2A] = {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* * */
    [0x2B] = {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, /* + */
    [0x2C] = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, /* , */
    [0x2D] = {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, /* - */
    [0x2E] = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, /* . */
    [0x2F] = {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, /* / */
    [0x30] = {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, /* 0 */
    [0x31] = {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, /* 1 */
    [0x32] = {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, /* 2 */
    [0x33] = {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, /* 3 */
    [0x34] = {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, /* 4 */
    [0x35] = {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, /* 5 */
    [0x36] = {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, /* 6 */
    [0x37] = {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, /* 7 */
    [0x38] = {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, /* 8 */
    [0x39] = {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, /* 9 */
    [0x3A] = {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, /* : */
    [0x3B] = {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, /* ; */
    [0x3C] = {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, /* < */
    [0x3D] = {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, /* = */
    [0x3E] = {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* > */
    [0x3F] = {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, /* ? */
    [0x40] = {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, /* @ */
    [0x41] = {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, /* A */
    [0x42] = {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, /* B */
    [0x43] = {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, /* C */
    [0x44] = {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, /* D */
    [0x45] = {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, /* E */
    [0x46] = {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, /* F */
    [0x47] = {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, /* G */
    [0x48] = {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, /* H */
    [0x49] = {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* I */
    [0x4A] = {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, /* J */
    [0x4B] = {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, /* K */
    [0x4C] = {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, /* L */
    [0x4D] = {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, /* M */
    [0x4E] = {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, /* N */
    [0x4F] = {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, /* O */
    [0x50] = {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, /* P */
    [0x51] = {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, /* Q */
    [0x52] = {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, /* R */
    [0x53] = {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, /* S */
    [0x54] = {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* T */
    [0x55] = {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, /* U */
    [0x56] = {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* V */
    [0x57] = {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, /* W */
    [0x58] = {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, /* X */
    [0x59] = {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, /* Y */
    [0x5A] = {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, /* Z */
    [0x5B] = {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, /* [ */
    [0x5C] = {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, /* \ */
    [0x5D] = {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, /* ] */
    [0x5E] = {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, /* ^ */
    [0x5F] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* _ */
    [0x60] = {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, /* ` */
    [0x61] = {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, /* a */
    [0x62] = {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, /* b */
    [0x63] = {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, /* c */
    [0x64] = {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00}, /* d */
    [0x65] = {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00}, /* e */
    [0x66] = {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00}, /* f */
    [0x67] = {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, /* g */
    [0x68] = {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, /* h */
    [0x69] = {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, /* i */
    [0x6A] = {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, /* j */
    [0x6B] = {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, /* k */
    [0x6C] = {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* l */
    [0x6D] = {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, /* m */
    [0x6E] = {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, /* n */
    [0x6F] = {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, /* o */
    [0x70] = {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, /* p */
    [0x71] = {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, /* q */
    [0x72] = {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, /* r */
    [0x73] = {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, /* s */
    [0x74] = {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, /* t */
    [0x75] = {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, /* u */
    [0x76] = {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* v */
    [0x77] = {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, /* w */
    [0x78] = {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, /* x */
    [0x79] = {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, /* y */
    [0x7A] = {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, /* z */
    [0x7B] = {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, /* { */
    [0x7C] = {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* | */
    [0x7D] = {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, /* } */
    [0x7E] = {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, /* ~ */
};

/* ---- 帧缓冲与几何 ---- */
static volatile uint32_t *g_fb = NULL;   /* 用户态帧缓冲映射（xRGB32） */
static uint32_t g_fb_width  = 0;
static uint32_t g_fb_height = 0;
static uint32_t g_fb_pitch  = 0;         /* 字节/行 */

/* 字形放大倍数：8x8 -> 16x16 */
#define CON_SCALE  2
#define CON_MARGIN 24
#define TITLE_H    40
#define CHAR_W     (8 * CON_SCALE)   /* 16 */
#define CHAR_H     (8 * CON_SCALE)   /* 16 */

/* 终端窗口客户区文本栅格 */
static uint32_t g_term_x = 0;   /* 列 */
static uint32_t g_term_y = 0;   /* 行 */
static uint32_t g_term_cols = 0;
static uint32_t g_term_rows = 0;

/* 颜色（xRGB32） */
#define COL_BG      0x002B2B3A
#define COL_DESKTOP 0x00101926
#define COL_BAR     0x003A2A4A
#define COL_WINBG   0x0014141C
#define COL_WINBORDER 0x00606080
#define COL_FG      0x00E0E0E8
#define COL_TITLE   0x00C0C0FF

/* 像素写入（含边界保护） */
static inline void put_px(uint32_t x, uint32_t y, uint32_t rgb)
{
    if (x >= g_fb_width || y >= g_fb_height) return;
    g_fb[y * (g_fb_pitch / 4) + x] = rgb;
}

/* 填充矩形 */
static void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb)
{
    for (uint32_t j = 0; j < h; j++) {
        for (uint32_t i = 0; i < w; i++) {
            put_px(x + i, y + j, rgb);
        }
    }
}

/*
 * 鼠标光标（Ring3 绘制，来自 mouse_server 经 DISPLAY_PORT 发的 MOUSE_MSG_*）。
 * 采用像素快照法：保存光标覆盖区域的背景，绘制前先恢复旧位置背景再画新位置，
 * 避免擦除破坏桌面/终端像素。光标为 12x18 简易箭头（xRGB32）。
 */
static int32_t  g_cur_x = -MOUSE_CURSOR_W;   /* 当前光标左上角（初始隐藏） */
static int32_t  g_cur_y = -MOUSE_CURSOR_H;
static bool     g_cur_visible = false;
static uint32_t g_cur_bg[MOUSE_CURSOR_W * MOUSE_CURSOR_H];

/* 12x18 光标位图：1=前景(白)，0=透明(取背景) */
static const uint8_t g_cursor_mask[MOUSE_CURSOR_H][MOUSE_CURSOR_W] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,0,0,0,0,0},
    {1,1,1,1,1,1,1,1,0,0,0,0},
    {1,1,1,1,1,1,1,1,1,0,0,0},
    {1,1,1,1,1,1,1,1,1,1,0,0},
    {1,1,1,1,1,1,1,1,1,1,1,0},
    {1,1,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,1,1,1,1,1,1,1,0,0},
    {1,1,1,1,1,1,1,1,0,0,0,0},
    {1,1,1,1,1,1,0,0,0,0,0,0},
    {1,1,1,1,1,0,0,0,0,0,0,0},
    {1,1,1,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
};

/* 把鼠标报来的绝对坐标（mouse_server 基于 1024x768 基准累计）clamp 到实际
 * 分辨率，防御越界写帧缓冲触发 #PF 或画面错位。 */
static int32_t clamp_cursor_x(int32_t x)
{
    if (x < 0) x = 0;
    if (x + MOUSE_CURSOR_W > (int32_t)g_fb_width)
        x = (int32_t)g_fb_width - MOUSE_CURSOR_W;
    if (x < 0) x = 0;
    return x;
}
static int32_t clamp_cursor_y(int32_t y)
{
    if (y < 0) y = 0;
    if (y + MOUSE_CURSOR_H > (int32_t)g_fb_height)
        y = (int32_t)g_fb_height - MOUSE_CURSOR_H;
    if (y < 0) y = 0;
    return y;
}

static void cursor_restore_bg(void)
{
    if (!g_cur_visible) {
        return;
    }
    for (int32_t j = 0; j < MOUSE_CURSOR_H; j++) {
        for (int32_t i = 0; i < MOUSE_CURSOR_W; i++) {
            int32_t x = g_cur_x + i;
            int32_t y = g_cur_y + j;
            if (x < 0 || y < 0 || (uint32_t)x >= g_fb_width || (uint32_t)y >= g_fb_height)
                continue;
            put_px((uint32_t)x, (uint32_t)y, g_cur_bg[j * MOUSE_CURSOR_W + i]);
        }
    }
    g_cur_visible = false;
}

static void cursor_draw(int32_t nx, int32_t ny)
{
    /* 越界保护：clamp 到屏幕内 */
    nx = clamp_cursor_x(nx);
    ny = clamp_cursor_y(ny);

    cursor_restore_bg();   /* 先恢复旧位置背景 */

    /* 快照新位置背景并绘制前景 */
    for (int32_t j = 0; j < MOUSE_CURSOR_H; j++) {
        for (int32_t i = 0; i < MOUSE_CURSOR_W; i++) {
            int32_t x = nx + i;
            int32_t y = ny + j;
            if ((uint32_t)x >= g_fb_width || (uint32_t)y >= g_fb_height)
                continue;
            g_cur_bg[j * MOUSE_CURSOR_W + i] = g_fb[y * (g_fb_pitch / 4) + x];
            if (g_cursor_mask[j][i]) {
                put_px((uint32_t)x, (uint32_t)y, 0x00FFFFFF); /* 白色箭头 */
            }
        }
    }
    g_cur_x = nx;
    g_cur_y = ny;
    g_cur_visible = true;
}

/* 画一个 8x8 字形的 2x 放大版本（左上角 ox,oy） */
static void draw_glyph(uint32_t ox, uint32_t oy, uint8_t ch, uint32_t fg)
{
    if (ch >= 128) ch = '?';
    const uint8_t *g = disp_font8x8[ch];
    for (uint32_t row = 0; row < 8; row++) {
        uint8_t bits = g[row];
        for (uint32_t col = 0; col < 8; col++) {
            if (bits & (1u << col)) {
                /* 2x 放大 */
                put_px(ox + col * CON_SCALE,     oy + row * CON_SCALE,     fg);
                put_px(ox + col * CON_SCALE + 1, oy + row * CON_SCALE,     fg);
                put_px(ox + col * CON_SCALE,     oy + row * CON_SCALE + 1, fg);
                put_px(ox + col * CON_SCALE + 1, oy + row * CON_SCALE + 1, fg);
            }
        }
    }
}

/* ---- ANSI/VT100 转义序列（CSI）解析状态 ----
 * shell 的行内编辑依赖控制序列（清行尾 ESC[K、光标移动 ESC[nD / ESC[nC、
 * 归位 ESC[H、清屏 ESC[2J）。若本服务把 '[' 'K' 'D' 等当普通字符栅格化，
 * 屏幕上就会出现 "[k" 之类的垃圾字符——故必须在此解析并执行这些序列。 */
#define CSI_NONE 0
#define CSI_ESC  1   /* 已收到 ESC */
#define CSI_BRK  2   /* 已收到 ESC [ （正在收集参数，等待终结字母） */
static int g_esc_state = CSI_NONE;
static int g_csi_n = 0;        /* 第一个数值参数 */
static int g_csi_has_n = 0;    /* 是否显式给出了参数 */

/* 清除第 row 行 [x0, x1) 列的像素（用窗口背景色填充） */
static void clear_cols(uint32_t row, uint32_t x0, uint32_t x1)
{
    if (x1 > g_term_cols) x1 = g_term_cols;
    if (x0 >= x1) return;
    uint32_t px = CON_MARGIN + x0 * CHAR_W;
    uint32_t py = TITLE_H + CON_MARGIN + row * CHAR_H;
    fill_rect(px, py, (x1 - x0) * CHAR_W, CHAR_H, COL_WINBG);
}

/* 清屏：只清终端客户区并让光标归位（保留桌面与标题栏） */
static void clear_screen_area(void)
{
    uint32_t wx = CON_MARGIN + 2;
    uint32_t wy = TITLE_H + CON_MARGIN + 2;
    uint32_t ww = g_fb_width  - CON_MARGIN * 2 - 4;
    uint32_t wh = g_fb_height - TITLE_H - CON_MARGIN * 2 - 4;
    fill_rect(wx, wy, ww, wh, COL_WINBG);
    g_term_x = 0;
    g_term_y = 0;
}

/* 终端软滚动：把客户区整体上移【一行】（CHAR_H 像素），末行清空。
 * 与 clear_screen_area 不同，本函数保留已有文本，仅把顶部一行推出窗口，
 * 避免 shell 写满时「刷新整个区域」（历史文本丢失、整屏闪烁）。
 * 像素拷贝直接在帧缓冲上做（xRGB32，每像素 4 字节）；拷贝方向自上而下、
 * 目标行恒在源行之上，逐行互不重叠，安全无撕裂。 */
static void term_scroll_up(void)
{
    if (g_term_rows == 0) return;
    uint32_t left     = CON_MARGIN;
    uint32_t top      = TITLE_H + CON_MARGIN;
    uint32_t width_px = g_fb_width - CON_MARGIN * 2;
    uint32_t pitch_px = g_fb_pitch / 4;
    uint32_t row_px   = CHAR_H;
    uint32_t rows     = g_term_rows;

    /* 鼠标光标若是可见的，先在滚动前恢复其背景：否则滚动把箭头像素一起上移，
     * 之后光标移动恢复背景时会把错误的像素写回，造成残影。 */
    if (g_cur_visible) cursor_restore_bg();

    /* 逐行上移：第 y 行 <- 第 y+1 行（y = 0 .. rows-2） */
    for (uint32_t y = 0; y + 1 < rows; y++) {
        uint32_t *dst = (uint32_t *)g_fb + (top + y * row_px) * pitch_px + left;
        uint32_t *src = (uint32_t *)g_fb + (top + (y + 1) * row_px) * pitch_px + left;
        memcpy(dst, src, (size_t)width_px * row_px * sizeof(uint32_t));
    }
    /* 清空末行（用窗口背景色填充） */
    uint32_t *dst = (uint32_t *)g_fb + (top + (rows - 1) * row_px) * pitch_px + left;
    for (uint32_t i = 0; i < width_px * row_px; i++) dst[i] = COL_WINBG;
}

/* 执行一条 CSI 序列（终结字母 final） */
static void csi_dispatch(char final)
{
    int n = g_csi_has_n ? g_csi_n : 1;   /* 缺省参数为 1（A/B/C/D） */
    switch (final) {
    case 'A':   /* 光标上移 */
        g_term_y = (g_term_y >= (uint32_t)n) ? g_term_y - (uint32_t)n : 0;
        break;
    case 'B':   /* 光标下移 */
        g_term_y += (uint32_t)n;
        if (g_term_y >= g_term_rows) g_term_y = g_term_rows ? g_term_rows - 1 : 0;
        break;
    case 'C':   /* 光标右移 */
        g_term_x += (uint32_t)n;
        if (g_term_x >= g_term_cols) g_term_x = g_term_cols ? g_term_cols - 1 : 0;
        break;
    case 'D':   /* 光标左移 */
        g_term_x = (g_term_x >= (uint32_t)n) ? g_term_x - (uint32_t)n : 0;
        break;
    case 'G':   /* 光标水平绝对定位 CHA：ESC[<col>G（1-based） */
        g_term_x = (g_csi_has_n && g_csi_n >= 1) ? (uint32_t)(g_csi_n - 1) : 0;
        if (g_term_x >= g_term_cols) g_term_x = g_term_cols ? g_term_cols - 1 : 0;
        break;
    case 'H':   /* 光标定位（本服务按归位处理：shell 只发 ESC[H） */
        g_term_x = 0;
        g_term_y = 0;
        break;
    case 'J':   /* 清屏：n=2 全清；n=0 清光标到屏尾；n=1 清屏首到光标 */
        if (g_csi_has_n && g_csi_n == 1 && g_term_y > 0) {
            for (uint32_t r = 0; r < g_term_y; r++) clear_cols(r, 0, g_term_cols);
            clear_cols(g_term_y, 0, g_term_x);
        } else {
            clear_screen_area();
        }
        break;
    case 'K':   /* 清行：n=0 光标到行尾；n=1 行首到光标；n=2 整行 */
        if (g_csi_has_n && g_csi_n == 2)      clear_cols(g_term_y, 0, g_term_cols);
        else if (g_csi_has_n && g_csi_n == 1) clear_cols(g_term_y, 0, g_term_x + 1);
        else                                  clear_cols(g_term_y, g_term_x, g_term_cols);
        break;
    case 'm':   /* SGR 颜色/属性：不支持，忽略 */
    default:
        break;
    }
}

/* 画一个字符到终端客户区当前光标位置，并推进光标 */
static void term_putc(char c)
{
    /* ---- 第一层：转义序列状态机（不绘制任何东西） ---- */
    if (g_esc_state == CSI_ESC) {
        if (c == '[') {
            g_esc_state = CSI_BRK;
            g_csi_n = 0;
            g_csi_has_n = 0;
        } else {
            g_esc_state = CSI_NONE;   /* ESC 后非 '['：整条序列忽略 */
        }
        return;
    }
    if (g_esc_state == CSI_BRK) {
        if (c >= '0' && c <= '9') {           /* 收集数值参数 */
            if (g_csi_n < 9999) g_csi_n = g_csi_n * 10 + (c - '0');
            g_csi_has_n = 1;
            return;
        }
        if (c == ';' || c == '?') return;     /* 参数分隔/私有前缀：忽略后续参数 */
        g_esc_state = CSI_NONE;
        if (c >= '@' && c <= '~') csi_dispatch(c);   /* 终结字母 */
        return;                                /* 未知/异常字节：放弃本条序列 */
    }
    if (c == 0x1B) { g_esc_state = CSI_ESC; return; }   /* ESC：进入转义 */

    /* ---- 第二层：原有普通字符处理 ---- */
    if (c == '\r') { g_term_x = 0; return; }
    if (c == '\n') { g_term_x = 0; g_term_y++; }
    else if (c == '\t') {
        uint32_t ntab = 8 - (g_term_x % 8);
        for (uint32_t i = 0; i < ntab; i++) term_putc(' ');
        return;
    }
    else if (c == '\b') {
        if (g_term_x > 0) g_term_x--;
        else if (g_term_y > 0) { g_term_y--; g_term_x = g_term_cols - 1; }
        else return;   /* 已在左上角，无法再退 */
        /* 退格除了移动光标，还要清除被删位置的像素，否则旧字符会残留在屏幕上。
         * 即使上层发送 "\b \b" 序列（空格负责清除），此处主动清格也无害且更健壮
         * （单发 \b 时也能正确清屏）。 */
        uint32_t px = CON_MARGIN + g_term_x * CHAR_W;
        uint32_t py = TITLE_H + CON_MARGIN + g_term_y * CHAR_H;
        fill_rect(px, py, CHAR_W, CHAR_H, COL_WINBG);
        return;
    }
    else {
        uint32_t px = CON_MARGIN + g_term_x * CHAR_W;
        uint32_t py = TITLE_H + CON_MARGIN + g_term_y * CHAR_H;
        draw_glyph(px, py, (uint8_t)c, COL_FG);
        g_term_x++;
    }
    if (g_term_x >= g_term_cols) { g_term_x = 0; g_term_y++; }
    if (g_term_y >= g_term_rows) {
        /* 软滚动：客户区上移一行（保留历史文本），而非清空整片区域。 */
        term_scroll_up();
        g_term_y = g_term_rows - 1;
        g_term_x = 0;
    }
}

/* 写字符串 */
static void term_puts(const char *s)
{
    for (; *s; s++) {
        term_putc(*s);
    }
}

/* 绘制桌面 + 标题栏 + 终端窗口 */
static void draw_desktop(void)
{
    fill_rect(0, 0, g_fb_width, g_fb_height, COL_DESKTOP);
    /* 标题栏 */
    fill_rect(0, 0, g_fb_width, TITLE_H, COL_BAR);
    /* 标题文字 */
    const char *title = "SukiOS";
    uint32_t tx = CON_MARGIN;
    for (const char *p = title; *p; p++) {
        draw_glyph(tx, (TITLE_H - CHAR_H) / 2, (uint8_t)*p, COL_TITLE);
        tx += CHAR_W;
    }
    /* 终端窗口边框 + 客户区 */
    uint32_t wx = CON_MARGIN, wy = TITLE_H + CON_MARGIN;
    uint32_t ww = g_fb_width  - CON_MARGIN * 2;
    uint32_t wh = g_fb_height - TITLE_H - CON_MARGIN * 2;
    fill_rect(wx, wy, ww, wh, COL_WINBORDER);
    fill_rect(wx + 2, wy + 2, ww - 4, wh - 4, COL_WINBG);

    g_term_cols = (ww - 4) / CHAR_W;
    g_term_rows = (wh - 4) / CHAR_H;
    g_term_x = 0; g_term_y = 0;
}

/* 启动时：把内核启动日志（SYS_CONSOLE_READ，类似 dmesg）刷到终端窗口 */
static void drain_console_pipe(void)
{
    static char buf[512];
    for (;;) {
        uint64_t n = suki_syscall2(SYS_CONSOLE_READ, (uint64_t)buf, sizeof(buf) - 1);
        if (n == (uint64_t)-1 || n == 0) break;
        buf[n] = '\0';
        term_puts(buf);
        if (n < sizeof(buf) - 1) break;   /* 一次性取完 */
    }
}

int main(void)
{
    /* 1) 认领显示端口（接收文本消息） */
    uint64_t rc = sys_port_claim(DISPLAY_PORT);
    if (rc != 0) {
        u_print("display: port claim failed\n");
        sys_exit(1);
    }

    /* 2) 取得帧缓冲用户态映射（内核已用 BGA 设到 1280x720@32bpp） */
    fb_map_result_t res;
    if (suki_syscall2(SYS_FRAMEBUFFER_MAP, (uint64_t)&res, 0) != 0 || !res.enabled) {
        u_print("display: framebuffer map failed\n");
        sys_exit(1);
    }
    g_fb        = (volatile uint32_t *)res.fb_user_va;
    g_fb_width  = res.width;
    g_fb_height = res.height;
    g_fb_pitch  = res.pitch;

    u_print("display: fb mapped 1280x720@32 at user va\n");

    /* 3) 绘制桌面 */
    draw_desktop();
    term_puts("SukiOS display server ready.\n");
    term_puts("Kernel boot log:\n");

    /* 4) 通知内核：显示服务已接管帧缓冲。此后内核诊断不再直接写屏，
     *    而是被捕获进内核环形管道（SYS_CONSOLE_READ 可读回）。 */
    suki_syscall1(SYS_DISPLAY_READY, 0);

    /* 5) 刷内核启动日志到桌面终端窗口 */
    drain_console_pipe();

    /* 6) 进入消息循环：接收经 DISPLAY_PORT 转发的文本与鼠标事件并渲染 */
    static uint8_t msgbuf[512];
    /* 鼠标事件消息布局（与 user/mouse_server.c 的 mouse_event_msg_t 一致） */
    typedef struct {
        mach_msg_header_t h;
        int32_t  x;
        int32_t  y;
        uint8_t  buttons;
        int8_t   wheel;
        uint8_t  _pad[3];
    } mouse_event_msg_t;
    for (;;) {
        uint64_t r = mach_msg_recv(msgbuf, sizeof(msgbuf), DISPLAY_PORT);
        if (r == 0) {
            mach_msg_header_t *h = (mach_msg_header_t *)msgbuf;
            if (h->msgh_id == DISP_MSG_TEXT) {
                char *text = (char *)msgbuf + sizeof(mach_msg_header_t);
                term_puts(text);
            } else if (h->msgh_id == MOUSE_MSG_MOVE ||
                       h->msgh_id == MOUSE_MSG_BUTTON ||
                       h->msgh_id == MOUSE_MSG_WHEEL) {
                mouse_event_msg_t *m = (mouse_event_msg_t *)msgbuf;
                /* 按钮状态：左键按下时画红色光标提示，否则白色（视觉反馈） */
                cursor_draw(m->x, m->y);
                if (m->buttons & 1) {
                    /* 左键按下：在光标尖端画一小红点（简单反馈） */
                    if ((uint32_t)m->x < g_fb_width && (uint32_t)m->y < g_fb_height)
                        put_px((uint32_t)m->x, (uint32_t)m->y, 0x00FF3030);
                }
            }
        }
        sys_yield();
    }
    return 0;
}
