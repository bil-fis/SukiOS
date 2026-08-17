/*
 * kernel/arch/x86_64/framebuffer.c
 * -----------------------------------------------------------------------------
 * 线性帧缓冲 (LFB) 驱动与图形文本控制台。
 *
 * 帧缓冲物理地址由 GRUB 通过 Multiboot2 提供（QEMU 通常 ~0xFD000000）。
 * 引导页表已映射 0..4GB，故通过 PHYS_TO_VIRT(fb_addr) 即可写显存。
 *
 * 调用关系：kmain() -> fb_init() -> fbcon_init() -> fbcon_write()。
 * 仅支持 32bpp（每像素 4 字节）；其它色深回退为不可用。
 */
#include <kernel/framebuffer.h>
#include <kernel/font.h>
#include <kernel/utf8.h>

#define CON_SCALE  2   /* 字形放大倍数：8x8 -> 16x16 */
#define CON_MARGIN 4   /* 边距像素 */

static struct {
    volatile uint8_t *base;   /* 显存虚拟基址 */
    uint32_t pitch;           /* 每行字节数 */
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
    bool     ready;
} g_fb;

/* 控制台状态 */
static struct {
    uint32_t cols, rows;
    uint32_t cx, cy;          /* 光标（字符坐标） */
    uint32_t fg, bg;
    uint32_t cw, ch;          /* 单字符像素宽高 */
} g_con;

/* UTF-8 解码状态（逐字节喂入，跨多次 fbcon_putc 保持） */
static utf8_state_t g_utf8;

/* 未知码点的兜底字形：16x16 虚线方框 */
static const uint8_t g_fallback_box[32] = {
    0xFF,0xFF, 0x81,0x81, 0x81,0x81, 0x81,0x81, 0x81,0x81, 0x81,0x81,
    0x81,0x81, 0x81,0x81, 0x81,0x81, 0x81,0x81, 0x81,0x81, 0x81,0x81,
    0x81,0x81, 0x81,0x81, 0x81,0x81, 0xFF,0xFF
};

/*
 * 取码点对应的字形。ASCII 用 8x8 放大显示，CJK 用 16x16，
 * 未知码点用兜底方框。始终成功（保证可渲染）。
 */
bool fb_get_glyph(uint32_t cp, glyph_t *g)
{
    if (cp >= 0x20 && cp < 0x80) {
        g->bits  = font8x8_basic[cp];
        g->w = 8; g->h = 8; g->stride = 1; g->scale = CON_SCALE;
        return true;
    }
    if (font_cjk_get(cp, &g->bits, &g->w, &g->h)) {
        g->stride = (g->w + 7) / 8;
        g->scale = 1;
        return true;
    }
    g->bits = g_fallback_box;
    g->w = 16; g->h = 16; g->stride = 2; g->scale = 1;
    return true;
}

bool fb_init(const boot_info_t *bi)
{
    if (!bi->fb_present || bi->fb_bpp != 32 || bi->fb_addr == 0) {
        g_fb.ready = false;
        return false;
    }
    g_fb.base   = (volatile uint8_t *)PHYS_TO_VIRT(bi->fb_addr);
    g_fb.pitch  = bi->fb_pitch;
    g_fb.width  = bi->fb_width;
    g_fb.height = bi->fb_height;
    g_fb.bpp    = bi->fb_bpp;
    g_fb.ready  = true;
    return true;
}

bool     fb_available(void) { return g_fb.ready; }
uint32_t fb_width(void)     { return g_fb.width; }
uint32_t fb_height(void)    { return g_fb.height; }

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (!g_fb.ready || x >= g_fb.width || y >= g_fb.height) {
        return;
    }
    volatile uint32_t *p =
        (volatile uint32_t *)(g_fb.base + (uint64_t)y * g_fb.pitch + (uint64_t)x * 4);
    *p = color;
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    if (!g_fb.ready) {
        return;
    }
    for (uint32_t j = 0; j < h; j++) {
        uint32_t yy = y + j;
        if (yy >= g_fb.height) {
            break;
        }
        volatile uint32_t *row =
            (volatile uint32_t *)(g_fb.base + (uint64_t)yy * g_fb.pitch + (uint64_t)x * 4);
        for (uint32_t i = 0; i < w; i++) {
            if (x + i >= g_fb.width) {
                break;
            }
            row[i] = color;
        }
    }
}

void fb_clear(uint32_t color)
{
    fb_fill_rect(0, 0, g_fb.width, g_fb.height, color);
}

void fb_draw_char(uint32_t px, uint32_t py, uint32_t cp, uint32_t fg, uint32_t bg)
{
    if (!g_fb.ready) {
        return;
    }
    glyph_t g;
    fb_get_glyph(cp, &g);
    /* 清除字符单元背景（按字形尺寸 * scale） */
    fb_fill_rect(px, py, g.w * g.scale, g.h * g.scale, bg);
    for (int row = 0; row < g.h; row++) {
        const uint8_t *line = g.bits + row * g.stride;
        for (int col = 0; col < g.w; col++) {
            uint8_t b = line[col >> 3];
            if (b & (1u << (col & 7))) {
                fb_fill_rect(px + col * g.scale, py + row * g.scale,
                             g.scale, g.scale, fg);
            }
        }
    }
}

/* --------------------------- 图形文本控制台 ------------------------------ */

void fbcon_init(void)
{
    if (!g_fb.ready) {
        return;
    }
    utf8_init(&g_utf8);        /* 重置 UTF-8 解码状态 */
    g_con.cw = FONT_WIDTH  * CON_SCALE;
    g_con.ch = FONT_HEIGHT * CON_SCALE;
    g_con.cols = (g_fb.width  - 2 * CON_MARGIN) / g_con.cw;
    g_con.rows = (g_fb.height - 2 * CON_MARGIN) / g_con.ch;
    g_con.cx = 0;
    g_con.cy = 0;
    g_con.fg = FB_WHITE;
    g_con.bg = FB_BG;
    fb_clear(FB_BG);
}

void fbcon_set_color(uint32_t fg, uint32_t bg)
{
    g_con.fg = fg;
    g_con.bg = bg;
}

static void fbcon_scroll(void)
{
    /* 整屏上移一个字符行：逐像素行拷贝，末行清空 */
    uint32_t line_h = g_con.ch;
    uint32_t top = CON_MARGIN;
    uint32_t used_h = g_con.rows * g_con.ch;
    for (uint32_t y = 0; y < used_h - line_h; y++) {
        volatile uint8_t *dst = g_fb.base + (uint64_t)(top + y) * g_fb.pitch;
        volatile uint8_t *src = g_fb.base + (uint64_t)(top + y + line_h) * g_fb.pitch;
        for (uint32_t b = 0; b < g_fb.width * 4; b++) {
            dst[b] = src[b];
        }
    }
    fb_fill_rect(CON_MARGIN, top + used_h - line_h, g_con.cols * g_con.cw, line_h, g_con.bg);
}

static void fbcon_newline(void)
{
    g_con.cx = 0;
    if (g_con.cy + 1 >= g_con.rows) {
        fbcon_scroll();
    } else {
        g_con.cy++;
    }
}

void fbcon_putc(char c)
{
    if (!g_fb.ready) {
        return;
    }
    /* 控制字符不进 UTF-8 解码，直接处理（保证退格/制表/换行正确） */
    switch (c) {
    case '\n': fbcon_newline(); return;
    case '\r': g_con.cx = 0; return;
    case '\t': for (int i = 0; i < 4; i++) fbcon_putc(' '); return;
    case '\b':
        if (g_con.cx > 0) {
            g_con.cx--;
            uint32_t px = CON_MARGIN + g_con.cx * g_con.cw;
            uint32_t py = CON_MARGIN + g_con.cy * g_con.ch;
            fb_fill_rect(px, py, g_con.cw, g_con.ch, g_con.bg);
        }
        return;
    }

    /* 其它字节喂入 UTF-8 解码器，得到完整 Unicode 码点后再渲染 */
    uint32_t cp;
    int r = utf8_feed(&g_utf8, (uint8_t)c, &cp);
    if (r == 0) {
        return;                 /* 续字节，等待更多字节 */
    }
    if (r < 0) {
        cp = 0xFFFD;            /* 非法序列 -> 替换符（走兜底方框） */
    }

    if (g_con.cx >= g_con.cols) {
        fbcon_newline();
    }
    uint32_t px = CON_MARGIN + g_con.cx * g_con.cw;
    uint32_t py = CON_MARGIN + g_con.cy * g_con.ch;
    fb_draw_char(px, py, cp, g_con.fg, g_con.bg);
    g_con.cx++;
}

void fbcon_write(const char *s)
{
    while (*s) {
        fbcon_putc(*s++);
    }
}
