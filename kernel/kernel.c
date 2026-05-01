/* =============================================================================
 * SukiOS - x86_64 内核入口（Multiboot2）
 *
 * 最小化内核入口，用于验证引导流程是否正常工作。
 * 使用 VGA 文本模式（0xB8000）输出信息，然后停机。
 *
 * 编译：
 *   gcc -c kernel.c -o kernel.o \
 *       -ffreestanding -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
 *       -mcmodel=large -O2 -Wall -Wextra -std=gnu11 -I include
 *
 * 参考：OSDev Bare Bones、Creating a 64-bit kernel
 * ============================================================================= */

#include <stdint.h>
#include "multiboot2.h"

/* 自定义 size_t（freestanding 环境无 <stddef.h>） */
typedef __SIZE_TYPE__ size_t;

/* 确保目标是 64 位 */
#if !defined(__x86_64__)
#error "本内核需要 x86_64 编译器。"
#endif

/* ================================================================== */
/*                       VGA 文本模式终端驱动                          */
/* ================================================================== */

#define VGA_WIDTH    80
#define VGA_HEIGHT   25
#define VGA_ADDR     0xB8000

/* VGA 颜色定义 */
#define VGA_BLACK       0
#define VGA_BLUE        1
#define VGA_GREEN       2
#define VGA_CYAN        3
#define VGA_RED         4
#define VGA_MAGENTA     5
#define VGA_BROWN       6
#define VGA_LIGHT_GREY  7
#define VGA_DARK_GREY   8
#define VGA_WHITE       15

static inline uint8_t vga_color(uint8_t fg, uint8_t bg)
{
    return fg | (bg << 4);
}

static inline uint16_t vga_entry(char c, uint8_t color)
{
    return (uint16_t)c | ((uint16_t)color << 8);
}

static size_t string_length(const char *str)
{
    size_t len = 0;
    while (str[len])
        len++;
    return len;
}

/* 终端状态 */
static size_t terminal_row;
static size_t terminal_col;
static uint8_t terminal_color;
static uint16_t *terminal_buffer;

/* 设置终端前景色 */
static void terminal_setcolor(uint8_t color)
{
    terminal_color = color;
}

/* 输出单个字符 */
static void terminal_putchar(char c)
{
    if (c == '\n') {
        terminal_col = 0;
        if (++terminal_row == VGA_HEIGHT)
            terminal_row = 0;
        return;
    }

    const size_t idx = terminal_row * VGA_WIDTH + terminal_col;
    terminal_buffer[idx] = vga_entry(c, terminal_color);

    if (++terminal_col == VGA_WIDTH) {
        terminal_col = 0;
        if (++terminal_row == VGA_HEIGHT)
            terminal_row = 0;
    }
}

/* 输出字符串 */
static void terminal_write(const char *data, size_t size)
{
    for (size_t i = 0; i < size; i++)
        terminal_putchar(data[i]);
}

static void terminal_print(const char *str)
{
    terminal_write(str, string_length(str));
}

/* 以十六进制输出 32 位无符号整数 */
static void terminal_print_hex32(uint32_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    terminal_print("0x");
    for (int i = 28; i >= 0; i -= 4)
        terminal_putchar(hex[(value >> i) & 0xF]);
}

/* 以十六进制输出 64 位无符号整数 */
static void terminal_print_hex64(uint64_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    terminal_print("0x");
    for (int i = 60; i >= 0; i -= 4)
        terminal_putchar(hex[(value >> i) & 0xF]);
}

/* 以十进制输出无符号整数 */
static void terminal_print_dec(uint32_t value)
{
    char buf[16];
    int idx = 15;
    buf[idx--] = '\0';
    if (value == 0) {
        terminal_putchar('0');
        return;
    }
    while (value) {
        buf[idx--] = '0' + (value % 10);
        value /= 10;
    }
    terminal_print(&buf[idx + 1]);
}

/* 初始化终端：清屏，设置默认颜色 */
static void terminal_initialize(void)
{
    terminal_row = 0;
    terminal_col = 0;
    terminal_color = vga_color(VGA_LIGHT_GREY, VGA_BLACK);
    terminal_buffer = (uint16_t *)VGA_ADDR;

    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            const size_t idx = y * VGA_WIDTH + x;
            terminal_buffer[idx] = vga_entry(' ', terminal_color);
        }
    }
}

/* ================================================================== */
/*                          内核入口                                   */
/* ================================================================== */

/*
 * kernel_main 由 boot.asm 调用，参数通过 System V AMD64 调用约定传递：
 *   RDI = multiboot2 魔数（0x36D76289）
 *   RSI = multiboot2 信息结构的物理地址
 */
void kernel_main(uint32_t magic, uint64_t mbi_addr)
{
    terminal_initialize();

    /* 校验 Multiboot2 魔数 */
    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
        terminal_setcolor(vga_color(VGA_RED, VGA_BLACK));
        terminal_print("SukiOS: Invalid Multiboot2 magic!\n");
        terminal_print("  Expected 0x36D76289, got ");
        terminal_print_hex32(magic);
        terminal_putchar('\n');
        goto halt;
    }

    /* 检查 Multiboot2 信息结构是否 8 字节对齐 */
    if (mbi_addr & 7) {
        terminal_setcolor(vga_color(VGA_RED, VGA_BLACK));
        terminal_print("SukiOS: MBI not aligned!\n");
        goto halt;
    }

    /* ---- 打印欢迎信息 ---- */
    terminal_setcolor(vga_color(VGA_GREEN, VGA_BLACK));
    terminal_print("  _____ _____ __  __  __  __ \n");
    terminal_print(" |  __ \\_   _|  \\/  | |  \\/  |\n");
    terminal_print(" | |__) || | | .  . | | .  . |\n");
    terminal_print(" |  ___/ | | | |\\/| | | |\\/| |\n");
    terminal_print(" | |    _| |_| |  | | | |  | |\n");
    terminal_print(" |_|   |_____|_|  |_|_|_|  |_|\n");
    terminal_print("\n");

    terminal_setcolor(vga_color(VGA_LIGHT_GREY, VGA_BLACK));
    terminal_print("SukiOS v0.1.0 - x86_64\n\n");

    /* ---- 打印引导信息 ---- */
    terminal_setcolor(vga_color(VGA_CYAN, VGA_BLACK));
    terminal_print("Boot info:\n");
    terminal_setcolor(vga_color(VGA_LIGHT_GREY, VGA_BLACK));
    terminal_print("  Magic: 0x36D76289 (Multiboot2 OK)\n");
    terminal_print("  MBI address: ");
    terminal_print_hex64(mbi_addr);
    terminal_putchar('\n');

    /* ---- 解析 Multiboot2 信息标签 ---- */
    terminal_setcolor(vga_color(VGA_CYAN, VGA_BLACK));
    terminal_print("\nMultiboot2 tags:\n");
    terminal_setcolor(vga_color(VGA_LIGHT_GREY, VGA_BLACK));

    struct multiboot2_tag *tag;
    for (tag = (struct multiboot2_tag *)(mbi_addr + 8);
         tag->type != MULTIBOOT2_TAG_END;
         tag = (struct multiboot2_tag *)((uint8_t *)tag + ((tag->size + 7) & ~7)))
    {
        switch (tag->type) {
        case MULTIBOOT2_TAG_CMDLINE:
            terminal_print("  [Cmdline] ");
            terminal_print(((struct multiboot2_tag_string *)tag)->string);
            terminal_putchar('\n');
            break;

        case MULTIBOOT2_TAG_BOOT_LOADER_NAME:
            terminal_print("  [Bootloader] ");
            terminal_print(((struct multiboot2_tag_string *)tag)->string);
            terminal_putchar('\n');
            break;

        case MULTIBOOT2_TAG_BASIC_MEMINFO: {
            struct multiboot2_tag_basic_meminfo *mem =
                (struct multiboot2_tag_basic_meminfo *)tag;
            terminal_print("  [Memory] lower: ");
            terminal_print_dec(mem->mem_lower);
            terminal_print(" KB, upper: ");
            terminal_print_dec(mem->mem_upper);
            terminal_print(" KB\n");
            break;
        }

        case MULTIBOOT2_TAG_MMAP: {
            struct multiboot2_tag_mmap *mmap_tag =
                (struct multiboot2_tag_mmap *)tag;
            terminal_print("  [Memory map] usable regions:\n");
            struct multiboot2_mmap_entry *entry = mmap_tag->entries;
            while ((uint8_t *)entry < (uint8_t *)tag + tag->size) {
                if (entry->type == MULTIBOOT_MEMORY_AVAILABLE && entry->len > 0) {
                    terminal_print("    ");
                    terminal_print_hex64(entry->addr);
                    terminal_print(" - ");
                    terminal_print_hex64(entry->addr + entry->len);
                    terminal_print(" (");
                    terminal_print_dec((uint32_t)(entry->len / 1024));
                    terminal_print(" KB)\n");
                }
                entry = (struct multiboot2_mmap_entry *)
                    ((uint8_t *)entry + mmap_tag->entry_size);
            }
            break;
        }

        case MULTIBOOT2_TAG_FRAMEBUFFER: {
            struct multiboot2_tag_framebuffer *fb =
                (struct multiboot2_tag_framebuffer *)tag;
            terminal_print("  [Framebuffer] ");
            terminal_print_dec(fb->framebuffer_width);
            terminal_print("x");
            terminal_print_dec(fb->framebuffer_height);
            terminal_print("x");
            terminal_print_dec(fb->framebuffer_bpp);
            terminal_print(" @ ");
            terminal_print_hex64(fb->framebuffer_addr);
            terminal_putchar('\n');
            break;
        }
        }
    }

    terminal_setcolor(vga_color(VGA_GREEN, VGA_BLACK));
    terminal_print("\nSukiOS booted successfully!\n");

halt:
    /* 停机：禁用中断并循环 halt */
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
