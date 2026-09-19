/*
 * include/kernel/input_pref.h
 * -----------------------------------------------------------------------------
 * 输入源优先级仲裁。
 *
 * 策略（用户要求）：**USB HID 键鼠优先**；USB 缺失 / 枚举失败 / 传输异常 /
 * 拔出时**回退 PS/2**。
 *
 * 机制：USB 侧与 PS/2 侧最终都汇入同一内核输入缓冲（kbd_feed_byte /
 * mouse_feed_byte）。为避免同时存在时「双份输入」，在 PS/2 IRQ 分发点
 * （keyboard.c 的 kbd_irq_handler）按本模块标志**丢弃** PS/2 字节：
 *   - USB 键盘就绪  -> 丢弃 PS/2 键盘字节；
 *   - USB 鼠标就绪  -> 丢弃 PS/2 鼠标字节；
 *   - USB 失效      -> 标志清零，PS/2 字节恢复放行（自动回退）。
 *
 * USB 核心每次轮询后依据设备表调用 set_*；标志为单字 bool，天然原子。
 */
#ifndef _SUKI_KERNEL_INPUT_PREF_H
#define _SUKI_KERNEL_INPUT_PREF_H

#include <kernel/types.h>

/* 由 USB 核心在每次轮询后设置：是否存在“已枚举且中断端点有效”的 USB 键/鼠。 */
void input_pref_set_usb_kbd(bool active);
void input_pref_set_usb_mouse(bool active);

/* 查询当前是否由 USB 键/鼠接管（PS/2 分发据此决定是否丢弃字节）。 */
bool input_pref_usb_kbd_active(void);
bool input_pref_usb_mouse_active(void);

#endif /* _SUKI_KERNEL_INPUT_PREF_H */
