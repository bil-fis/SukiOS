/* =============================================================================
 * SukiOS - VGA 文本模式终端驱动
 * ============================================================================= */

#include "kernel/tty.h"
#include "kernel/uart.h"

#define VGA_WIDTH    80
#define VGA_HEIGHT   25
#define VGA_ADDR     0xB8000

static size_t term_row;
static size_t term_col;
static uint8_t term_color;
static uint16_t *term_buffer;

static inline uint8_t vga_color(uint8_t fg, uint8_t bg)
{
    return fg | (bg << 4);
}

static inline uint16_t vga_entry(char c, uint8_t color)
{
    return (uint16_t)c | ((uint16_t)color << 8);
}

/* 检测 VGA 硬件光标支持并启用 */
static inline void tty_update_cursor(void)
{
    uint16_t pos = (uint16_t)(term_row * VGA_WIDTH + term_col);
    outb(0x3D4, 14);
    outb(0x3D5, (uint8_t)(pos >> 8));
    outb(0x3D4, 15);
    outb(0x3D5, (uint8_t)pos);
}

/* 屏幕上滚一行 */
static void tty_scroll(void)
{
    /* 将第 1~24 行整体上移一行 */
    for (size_t y = 0; y < VGA_HEIGHT - 1; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            term_buffer[y * VGA_WIDTH + x] =
                term_buffer[(y + 1) * VGA_WIDTH + x];

    /* 最后一行用空格填充 */
    for (size_t x = 0; x < VGA_WIDTH; x++)
        term_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ', term_color);
}

void tty_init(void)
{
    /* 先初始化串口，再初始化 VGA */
    uart_init();

    term_row = 0;
    term_col = 0;
    term_color = vga_color(VGA_LIGHT_GREY, VGA_BLACK);
    term_buffer = (uint16_t *)VGA_ADDR;

    for (size_t y = 0; y < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            term_buffer[y * VGA_WIDTH + x] = vga_entry(' ', term_color);

    tty_update_cursor();
}

void tty_setcolor(uint8_t fg, uint8_t bg)
{
    term_color = vga_color(fg, bg);
}

void tty_putchar(char c)
{
    /* 同步输出到 COM1 串口 */
    uart_putchar(c);

    if (c == '\n') {
        term_col = 0;
        if (++term_row == VGA_HEIGHT) {
            term_row = VGA_HEIGHT - 1;
            tty_scroll();
        }
        tty_update_cursor();
        return;
    }

    term_buffer[term_row * VGA_WIDTH + term_col] = vga_entry(c, term_color);

    if (++term_col == VGA_WIDTH) {
        term_col = 0;
        if (++term_row == VGA_HEIGHT) {
            term_row = VGA_HEIGHT - 1;
            tty_scroll();
        }
    }

    tty_update_cursor();
}

void tty_print(const char *s)
{
    for (size_t i = 0; s[i]; i++)
        tty_putchar(s[i]);
}

void tty_print_hex64(uint64_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    tty_print("0x");
    for (int i = 60; i >= 0; i -= 4)
        tty_putchar(hex[(value >> i) & 0xF]);
}

void tty_print_dec(uint32_t value)
{
    if (value == 0) { tty_putchar('0'); return; }

    char buf[16];
    int idx = 15;
    buf[idx--] = '\0';
    while (value) {
        buf[idx--] = '0' + (value % 10);
        value /= 10;
    }
    tty_print(&buf[idx + 1]);
}
