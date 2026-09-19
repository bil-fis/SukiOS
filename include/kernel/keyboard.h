/*
 * include/kernel/keyboard.h
 * -----------------------------------------------------------------------------
 * PS/2 键盘 (IRQ1) 驱动。
 *
 * 手册 6.3：内核不解析扫描码，仅采集原始 [make/break][scancode]。阶段三先
 * 在内核内做最小 US 布局解析以便早期交互测试；阶段七 IPC 就绪后改为把原始
 * 扫描码打包发送给用户态 INPUT_SERVER (端口 0x1003)。
 */
#ifndef _SUKI_KERNEL_KEYBOARD_H
#define _SUKI_KERNEL_KEYBOARD_H

#include <kernel/types.h>

void keyboard_init(void);

/* 从内核输入环形缓冲取一个 ASCII 字符，无输入返回 0（非阻塞） */
char keyboard_getchar(void);

/* 取一个原始扫描码；无数据返回 -1（供 sys_input_read / INPUT_SERVER） */
int keyboard_get_scancode(void);

/* 向键盘输入环形缓冲注入一个原始 PS/2 set-1 扫描码（供 USB HID 键盘转译注入）。 */
void kbd_feed_byte(uint8_t sc);

#endif /* _SUKI_KERNEL_KEYBOARD_H */
