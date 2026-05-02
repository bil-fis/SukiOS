/* =============================================================================
 * SukiOS - PS/2 键盘驱动
 *
 * 参考：OSDev PS/2 Keyboard, 8042 PS/2 Controller
 *
 * 使用 Scancode Set 1（默认），通过 IRQ1 (IOAPIC 引脚 1) 接收中断。
 * 支持所有标准按键和扩展按键（E0 前缀）。
 * ============================================================================= */

#ifndef SUKIOS_KEYBOARD_H
#define SUKIOS_KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

/* ---- 键码定义 ---- */

/* 基本键码（无 E0 前缀） */
typedef enum {
    KEY_NONE = 0,
    KEY_ESCAPE      = 0x01,
    KEY_1           = 0x02,
    KEY_2           = 0x03,
    KEY_3           = 0x04,
    KEY_4           = 0x05,
    KEY_5           = 0x06,
    KEY_6           = 0x07,
    KEY_7           = 0x08,
    KEY_8           = 0x09,
    KEY_9           = 0x0A,
    KEY_0           = 0x0B,
    KEY_MINUS       = 0x0C,   /* - */
    KEY_EQUALS      = 0x0D,   /* = */
    KEY_BACKSPACE   = 0x0E,
    KEY_TAB         = 0x0F,
    KEY_Q           = 0x10,
    KEY_W           = 0x11,
    KEY_E           = 0x12,
    KEY_R           = 0x13,
    KEY_T           = 0x14,
    KEY_Y           = 0x15,
    KEY_U           = 0x16,
    KEY_I           = 0x17,
    KEY_O           = 0x18,
    KEY_P           = 0x19,
    KEY_LBRACKET    = 0x1A,   /* [ */
    KEY_RBRACKET    = 0x1B,   /* ] */
    KEY_ENTER       = 0x1C,
    KEY_LCTRL       = 0x1D,
    KEY_A           = 0x1E,
    KEY_S           = 0x1F,
    KEY_D           = 0x20,
    KEY_F           = 0x21,
    KEY_G           = 0x22,
    KEY_H           = 0x23,
    KEY_J           = 0x24,
    KEY_K           = 0x25,
    KEY_L           = 0x26,
    KEY_SEMICOLON   = 0x27,   /* ; */
    KEY_APOSTROPHE  = 0x28,   /* ' */
    KEY_GRAVE       = 0x29,   /* ` */
    KEY_LSHIFT      = 0x2A,
    KEY_BACKSLASH   = 0x2B,   /* \ */
    KEY_Z           = 0x2C,
    KEY_X           = 0x2D,
    KEY_C           = 0x2E,
    KEY_V           = 0x2F,
    KEY_B           = 0x30,
    KEY_N           = 0x31,
    KEY_M           = 0x32,
    KEY_COMMA       = 0x33,   /* , */
    KEY_DOT         = 0x34,   /* . */
    KEY_SLASH       = 0x35,   /* / */
    KEY_RSHIFT      = 0x36,
    KEY_KP_MULTIPLY = 0x37,   /* * */
    KEY_LALT        = 0x38,
    KEY_SPACE       = 0x39,
    KEY_CAPSLOCK    = 0x3A,
    KEY_F1          = 0x3B,
    KEY_F2          = 0x3C,
    KEY_F3          = 0x3D,
    KEY_F4          = 0x3E,
    KEY_F5          = 0x3F,
    KEY_F6          = 0x40,
    KEY_F7          = 0x41,
    KEY_F8          = 0x42,
    KEY_F9          = 0x43,
    KEY_F10         = 0x44,
    KEY_NUMLOCK     = 0x45,
    KEY_SCROLLLOCK  = 0x46,
    KEY_KP_7        = 0x47,
    KEY_KP_8        = 0x48,
    KEY_KP_9        = 0x49,
    KEY_KP_MINUS    = 0x4A,
    KEY_KP_4        = 0x4B,
    KEY_KP_5        = 0x4C,
    KEY_KP_6        = 0x4D,
    KEY_KP_PLUS     = 0x4E,
    KEY_KP_1        = 0x4F,
    KEY_KP_2        = 0x50,
    KEY_KP_3        = 0x51,
    KEY_KP_0        = 0x52,
    KEY_KP_DOT      = 0x53,
    KEY_F11         = 0x57,
    KEY_F12         = 0x58,

    /* ---- 扩展键码（E0 前缀） ---- */
    KEY_KP_ENTER    = 0x60,
    KEY_RCTRL       = 0x61,
    KEY_KP_DIVIDE   = 0x62,
    KEY_RALT        = 0x63,
    KEY_PAUSE       = 0x64,
    KEY_HOME        = 0x66,
    KEY_UP          = 0x67,
    KEY_PAGEUP      = 0x68,
    KEY_LEFT        = 0x69,
    KEY_RIGHT       = 0x6A,
    KEY_END         = 0x6B,
    KEY_DOWN        = 0x6C,
    KEY_PAGEDOWN    = 0x6D,
    KEY_INSERT      = 0x6E,
    KEY_DELETE      = 0x6F,

    KEY_LWIN        = 0x70,
    KEY_RWIN        = 0x71,
    KEY_MENU        = 0x72,
} keycode_t;

/* 键盘事件类型 */
typedef enum {
    KEY_EVENT_PRESS,
    KEY_EVENT_RELEASE,
} key_event_t;

/* 键盘事件 */
struct keyboard_event {
    keycode_t key;
    key_event_t type;
    bool     extended;     /* 是否有 E0 前缀 */
};

/* 键盘状态 */
struct keyboard_state {
    bool lshift, rshift;
    bool lctrl, rctrl;
    bool lalt, ralt;
    bool capslock;
    bool numlock;
    bool scrolllock;
};

/**
 * keyboard_init - 初始化 PS/2 键盘
 *
 * 步骤（参考 OSDev PS/2 Keyboard）：
 *   1. 检测 PS/2 控制器
 *   2. 配置 IOAPIC IRQ1 -> 向量 IRQ_KEYBOARD
 *   3. 使能键盘
 */
void keyboard_init(void);

/**
 * keyboard_getchar - 将当前按键转换为 ASCII 字符
 * @event: 键盘事件
 * @return: ASCII 字符，无对应字符返回 0
 */
char keyboard_getchar(const struct keyboard_event *event);

/**
 * keyboard_get_scancode_name - 获取键码名称（调试用）
 */
const char *keyboard_get_scancode_name(keycode_t key);

/* IRQ1 中断处理函数（由 idt_stubs.asm 调用） */
void keyboard_irq_handler(uint8_t int_no);

#endif /* SUKIOS_KEYBOARD_H */
