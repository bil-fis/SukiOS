/*
 * kernel/arch/x86_64/vga_text.c
 * -----------------------------------------------------------------------------
 * VGA 文本模式驱动，直接写 0xB8000 显存（每字符 2 字节：ASCII + 属性）。
 * 通过高半区恒等偏移 PHYS_TO_VIRT(0xB8000) 访问。
 */
#include <kernel/vga_text.h>

#define VGA_PHYS  0xB8000
#define VGA_ATTR  0x0F   /* 黑底白字 */

static volatile uint16_t *g_vga = (volatile uint16_t *)PHYS_TO_VIRT(VGA_PHYS);
static uint32_t g_row = 0, g_col = 0;

void vga_init(void)
{
    g_row = 0;
    g_col = 0;
    for (uint32_t i = 0; i < VGA_COLS * VGA_ROWS; i++) {
        g_vga[i] = ((uint16_t)VGA_ATTR << 8) | ' ';
    }
}

static void vga_scroll(void)
{
    for (uint32_t r = 1; r < VGA_ROWS; r++) {
        for (uint32_t c = 0; c < VGA_COLS; c++) {
            g_vga[(r - 1) * VGA_COLS + c] = g_vga[r * VGA_COLS + c];
        }
    }
    for (uint32_t c = 0; c < VGA_COLS; c++) {
        g_vga[(VGA_ROWS - 1) * VGA_COLS + c] = ((uint16_t)VGA_ATTR << 8) | ' ';
    }
    g_row = VGA_ROWS - 1;
}

void vga_putc(char c)
{
    if (c == '\n') {
        g_col = 0;
        if (++g_row >= VGA_ROWS) {
            vga_scroll();
        }
        return;
    }
    if (c == '\r') {
        g_col = 0;
        return;
    }
    g_vga[g_row * VGA_COLS + g_col] = ((uint16_t)VGA_ATTR << 8) | (uint8_t)c;
    if (++g_col >= VGA_COLS) {
        g_col = 0;
        if (++g_row >= VGA_ROWS) {
            vga_scroll();
        }
    }
}

void vga_write(const char *s)
{
    while (*s) {
        vga_putc(*s++);
    }
}
