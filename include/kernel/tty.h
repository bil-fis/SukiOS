/* =============================================================================
 * SukiOS - VGA 文本模式终端驱动
 * ============================================================================= */

#ifndef SUKIOS_TTY_H
#define SUKIOS_TTY_H

#include <stdint.h>
#include <stddef.h>
#include "kernel/io.h"

/* VGA 颜色 */
#define VGA_BLACK       0
#define VGA_BLUE        1
#define VGA_GREEN       2
#define VGA_CYAN        3
#define VGA_RED         4
#define VGA_MAGENTA     5
#define VGA_BROWN       6
#define VGA_LIGHT_GREY  7
#define VGA_DARK_GREY   8
#define VGA_YELLOW      14
#define VGA_WHITE       15

void tty_init(void);
void tty_putchar(char c);
void tty_print(const char *str);
void tty_setcolor(uint8_t fg, uint8_t bg);
void tty_print_hex64(uint64_t value);
void tty_print_dec(uint32_t value);

#endif /* SUKIOS_TTY_H */
