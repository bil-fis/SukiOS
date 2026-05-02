/* =============================================================================
 * SukiOS - PS/2 键盘驱动实现
 *
 * 参考：OSDev PS/2 Keyboard, 8042 PS/2 Controller
 *
 * Scancode Set 1 解码：
 *   - 单字节：普通键的按下/释放
 *   - E0 前缀：扩展键（方向键、编辑键等）
 *   - E1 前缀：Pause 键（E1 14 77 E1 F0 14 F0 77）
 *
 * 中断流程：
 *   IRQ1 -> IOAPIC -> LAPIC -> keyboard_irq_handler() -> scancode 解码
 * ============================================================================= */

#include "kernel/keyboard.h"
#include "kernel/apic.h"
#include "kernel/acpi.h"
#include "kernel/tty.h"
#include "kernel/io.h"
#include <stdint.h>
#include <stdbool.h>

/* ---- PS/2 控制器端口 ---- */
#define PS2_DATA_PORT   0x60    /* 数据端口（读/写） */
#define PS2_CMD_PORT     0x64    /* 命令端口（只写） */
#define PS2_STATUS_PORT  0x64    /* 状态端口（只读） */

/* ---- 全局键盘状态 ---- */
static struct keyboard_state kbd_state;

/* Scancode 状态机 */
static bool scancode_extended;  /* 等待 E0 扩展码 */

/* =========================================================================
 * PS/2 控制器辅助函数
 * 参考：OSDev 8042 PS/2 Controller
 * ========================================================================= */

static inline void ps2_wait_write(void)
{
    while (inb(PS2_STATUS_PORT) & 2)
        ;
}

static inline void ps2_wait_read(void)
{
    while (!(inb(PS2_STATUS_PORT) & 1))
        ;
}

static void keyboard_send_cmd(uint8_t cmd)
{
    ps2_wait_write();
    outb(PS2_DATA_PORT, cmd);
}

/* =========================================================================
 * Scancode Set 1 → ASCII 映射表（US QWERTY 布局）
 *
 * 使用 [index] = value 显式索引语法，避免条目数不一致导致的偏移错误。
 * 参考：OSDev PS/2 Keyboard - Scancode Set 1
 * ========================================================================= */

/* 无 Shift 的 ASCII 映射 */
static const char scancode_to_ascii[128] = {
    /* 0x00 */ 0,
    /* 0x01 Esc */ 0,
    /* 0x02 1 */ '1',   /* 0x03 2 */ '2',   /* 0x04 3 */ '3',
    /* 0x05 4 */ '4',   /* 0x06 5 */ '5',   /* 0x07 6 */ '6',
    /* 0x08 7 */ '7',   /* 0x09 8 */ '8',   /* 0x0A 9 */ '9',
    /* 0x0B 0 */ '0',
    /* 0x0C - */ '-',   /* 0x0D = */ '=',
    /* 0x0E Bksp */ '\b',
    /* 0x0F Tab */ '\t',
    /* 0x10 Q */ 'q',   /* 0x11 W */ 'w',   /* 0x12 E */ 'e',
    /* 0x13 R */ 'r',   /* 0x14 T */ 't',   /* 0x15 Y */ 'y',
    /* 0x16 U */ 'u',   /* 0x17 I */ 'i',   /* 0x18 O */ 'o',
    /* 0x19 P */ 'p',
    /* 0x1A [ */ '[',   /* 0x1B ] */ ']',
    /* 0x1C Enter */ '\n',
    /* 0x1D LCtrl */ 0,
    /* 0x1E A */ 'a',   /* 0x1F S */ 's',
    /* 0x20 D */ 'd',   /* 0x21 F */ 'f',   /* 0x22 G */ 'g',
    /* 0x23 H */ 'h',   /* 0x24 J */ 'j',   /* 0x25 K */ 'k',
    /* 0x26 L */ 'l',
    /* 0x27 ; */ ';',   /* 0x28 ' */ '\'',
    /* 0x29 ` */ '`',
    /* 0x2A LShift */ 0,
    /* 0x2B \ */ '\\',
    /* 0x2C Z */ 'z',   /* 0x2D X */ 'x',   /* 0x2E C */ 'c',
    /* 0x2F V */ 'v',   /* 0x30 B */ 'b',   /* 0x31 N */ 'n',
    /* 0x32 M */ 'm',
    /* 0x33 , */ ',',   /* 0x34 . */ '.',   /* 0x35 / */ '/',
    /* 0x36 RShift */ 0,
    /* 0x37 KP* */ 0,
    /* 0x38 LAlt */ 0,
    /* 0x39 Space */ ' ',
    /* 0x3A CapsLock */ 0,
    /* 0x3B-0x44 F1-F10 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x45 NumLock */ 0,  /* 0x46 ScrollLock */ 0,
    /* 0x47 KP7 */ '7',  /* 0x48 KP8 */ '8',  /* 0x49 KP9 */ '9',
    /* 0x4A KP- */ '-',
    /* 0x4B KP4 */ '4',  /* 0x4C KP5 */ '5',  /* 0x4D KP6 */ '6',
    /* 0x4E KP+ */ '+',
    /* 0x4F KP1 */ '1',  /* 0x50 KP2 */ '2',  /* 0x51 KP3 */ '3',
    /* 0x52 KP0 */ '0',  /* 0x53 KP. */ '.',
    /* 0x54-0x56 */ 0, 0, 0,
    /* 0x57 F11 */ 0,    /* 0x58 F12 */ 0,
};

/* Shift 修饰的 ASCII 映射 */
static const char scancode_to_ascii_shift[128] = {
    /* 0x00 */ 0,
    /* 0x01 Esc */ 0,
    /* 0x02 1 */ '!',   /* 0x03 2 */ '@',   /* 0x04 3 */ '#',
    /* 0x05 4 */ '$',   /* 0x06 5 */ '%',   /* 0x07 6 */ '^',
    /* 0x08 7 */ '&',   /* 0x09 8 */ '*',   /* 0x0A 9 */ '(',
    /* 0x0B 0 */ ')',
    /* 0x0C - */ '_',   /* 0x0D = */ '+',
    /* 0x0E Bksp */ '\b',
    /* 0x0F Tab */ '\t',
    /* 0x10 Q */ 'Q',   /* 0x11 W */ 'W',   /* 0x12 E */ 'E',
    /* 0x13 R */ 'R',   /* 0x14 T */ 'T',   /* 0x15 Y */ 'Y',
    /* 0x16 U */ 'U',   /* 0x17 I */ 'I',   /* 0x18 O */ 'O',
    /* 0x19 P */ 'P',
    /* 0x1A [ */ '{',   /* 0x1B ] */ '}',
    /* 0x1C Enter */ '\n',
    /* 0x1D LCtrl */ 0,
    /* 0x1E A */ 'A',   /* 0x1F S */ 'S',
    /* 0x20 D */ 'D',   /* 0x21 F */ 'F',   /* 0x22 G */ 'G',
    /* 0x23 H */ 'H',   /* 0x24 J */ 'J',   /* 0x25 K */ 'K',
    /* 0x26 L */ 'L',
    /* 0x27 ; */ ':',   /* 0x28 ' */ '"',
    /* 0x29 ` */ '~',
    /* 0x2A LShift */ 0,
    /* 0x2B \ */ '|',
    /* 0x2C Z */ 'Z',   /* 0x2D X */ 'X',   /* 0x2E C */ 'C',
    /* 0x2F V */ 'V',   /* 0x30 B */ 'B',   /* 0x31 N */ 'N',
    /* 0x32 M */ 'M',
    /* 0x33 , */ '<',   /* 0x34 . */ '>',   /* 0x35 / */ '?',
    /* 0x36 RShift */ 0,
    /* 0x37 KP* */ 0,
    /* 0x38 LAlt */ 0,
    /* 0x39 Space */ ' ',
    /* 0x3A CapsLock */ 0,
    /* 0x3B-0x44 F1-F10 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x45 NumLock */ 0,  /* 0x46 ScrollLock */ 0,
    /* 0x47 KP7 */ '7',  /* 0x48 KP8 */ '8',  /* 0x49 KP9 */ '9',
    /* 0x4A KP- */ '-',
    /* 0x4B KP4 */ '4',  /* 0x4C KP5 */ '5',  /* 0x4D KP6 */ '6',
    /* 0x4E KP+ */ '+',
    /* 0x4F KP1 */ '1',  /* 0x50 KP2 */ '2',  /* 0x51 KP3 */ '3',
    /* 0x52 KP0 */ '0',  /* 0x53 KP. */ '.',
    /* 0x54-0x56 */ 0, 0, 0,
    /* 0x57 F11 */ 0,    /* 0x58 F12 */ 0,
};

/* NumLock 状态下小键盘的 ASCII 映射（无 Shift） */
static const char scancode_to_ascii_numlock[128] = {
    /* 0x47 KP7 */ '7',  /* 0x48 KP8 */ '8',  /* 0x49 KP9 */ '9',
    /* 0x4A KP- */ '-',
    /* 0x4B KP4 */ '4',  /* 0x4C KP5 */ '5',  /* 0x4D KP6 */ '6',
    /* 0x4E KP+ */ '+',
    /* 0x4F KP1 */ '1',  /* 0x50 KP2 */ '2',  /* 0x51 KP3 */ '3',
    /* 0x52 KP0 */ '0',  /* 0x53 KP. */ '.',
};

/* 判断 scancode 是否为字母键 (a-z, A-Z) */
static inline bool is_letter_key(keycode_t key)
{
    /* QWERTY 行: Q-P (0x10-0x19)
     * Home 行: A-L (0x1E-0x26)
     * Bottom 行: Z-M (0x2C-0x32) */
    if (key >= 0x10 && key <= 0x19) return true;
    if (key >= 0x1E && key <= 0x26) return true;
    if (key >= 0x2C && key <= 0x32) return true;
    return false;
}

/* 判断 scancode 是否为小键盘键 (0x47-0x53) */
static inline bool is_kp_key(keycode_t key)
{
    return (key >= 0x47 && key <= 0x53);
}

/* =========================================================================
 * 公共接口
 * ========================================================================= */

char keyboard_getchar(const struct keyboard_event *event)
{
    keycode_t key = event->key;

    /* 修饰键、功能键不产生 ASCII */
    if (key == KEY_LSHIFT || key == KEY_RSHIFT ||
        key == KEY_LCTRL  || key == KEY_RCTRL  ||
        key == KEY_LALT   || key == KEY_RALT   ||
        key == KEY_CAPSLOCK || key == KEY_NUMLOCK || key == KEY_SCROLLLOCK ||
        key == KEY_F1 || key == KEY_F2 || key == KEY_F3 || key == KEY_F4 ||
        key == KEY_F5 || key == KEY_F6 || key == KEY_F7 || key == KEY_F8 ||
        key == KEY_F9 || key == KEY_F10 || key == KEY_F11 || key == KEY_F12 ||
        key == KEY_ESCAPE || key == KEY_NONE)
        return 0;

    /* 扩展键（E0 前缀）不产生 ASCII */
    if (event->extended)
        return 0;

    /* 仅处理按下事件 */
    if (event->type != KEY_EVENT_PRESS)
        return 0;

    bool shift = kbd_state.lshift || kbd_state.rshift;
    bool caps  = kbd_state.capslock;

    /* ---- 字母键：受 CapsLock + Shift 双重影响 ---- */
    if (is_letter_key(key)) {
        char c = scancode_to_ascii[key];  /* 小写字母 */
        if (!c) return 0;

        /* CapsLock 和 Shift 异或：两者同时按下或同时未按下时小写 */
        if (caps ^ shift)
            return c - 32;  /* 转大写 */
        return c;           /* 小写 */
    }

    /* ---- 小键盘数字键：受 NumLock 影响 ---- */
    if (is_kp_key(key)) {
        if (kbd_state.numlock)
            return scancode_to_ascii_numlock[key];
        /* NumLock 关闭时小键盘作为导航键，不产生 ASCII */
        return 0;
    }

    /* ---- 其他可打印键：仅受 Shift 影响 ---- */
    if (shift)
        return scancode_to_ascii_shift[key];
    return scancode_to_ascii[key];
}

const char *keyboard_get_scancode_name(keycode_t key)
{
    static const char *names[] = {
        [0x00] = "None",
        [0x01] = "Esc", [0x02] = "1", [0x03] = "2", [0x04] = "3",
        [0x05] = "4", [0x06] = "5", [0x07] = "6", [0x08] = "7",
        [0x09] = "8", [0x0A] = "9", [0x0B] = "0", [0x0C] = "-",
        [0x0D] = "=", [0x0E] = "Bksp", [0x0F] = "Tab",
        [0x10] = "Q", [0x11] = "W", [0x12] = "E", [0x13] = "R",
        [0x14] = "T", [0x15] = "Y", [0x16] = "U", [0x17] = "I",
        [0x18] = "O", [0x19] = "P", [0x1A] = "[", [0x1B] = "]",
        [0x1C] = "Enter", [0x1D] = "LCtrl", [0x1E] = "A", [0x1F] = "S",
        [0x20] = "D", [0x21] = "F", [0x22] = "G", [0x23] = "H",
        [0x24] = "J", [0x25] = "K", [0x26] = "L", [0x27] = ";",
        [0x28] = "'", [0x29] = "`", [0x2A] = "LShift", [0x2B] = "\\",
        [0x2C] = "Z", [0x2D] = "X", [0x2E] = "C", [0x2F] = "V",
        [0x30] = "B", [0x31] = "N", [0x32] = "M", [0x33] = ",",
        [0x34] = ".", [0x35] = "/", [0x36] = "RShift", [0x37] = "KP*",
        [0x38] = "LAlt", [0x39] = "Space", [0x3A] = "Caps", [0x3B] = "F1",
        [0x3C] = "F2", [0x3D] = "F3", [0x3E] = "F4", [0x3F] = "F5",
        [0x40] = "F6", [0x41] = "F7", [0x42] = "F8", [0x43] = "F9",
        [0x44] = "F10", [0x45] = "NumLk", [0x46] = "ScrLk", [0x47] = "KP7",
        [0x48] = "KP8", [0x49] = "KP9", [0x4A] = "KP-", [0x4B] = "KP4",
        [0x4C] = "KP5", [0x4D] = "KP6", [0x4E] = "KP+", [0x4F] = "KP1",
        [0x50] = "KP2", [0x51] = "KP3", [0x52] = "KP0", [0x53] = "KP.",
        [0x57] = "F11", [0x58] = "F12",
        [0x60] = "KPEnt", [0x61] = "RCtrl", [0x62] = "KP/",
        [0x63] = "RAlt", [0x64] = "Pause", [0x66] = "Home",
        [0x67] = "Up", [0x68] = "PgUp", [0x69] = "Left",
        [0x6A] = "Right", [0x6B] = "End", [0x6C] = "Down",
        [0x6D] = "PgDn", [0x6E] = "Ins", [0x6F] = "Del",
        [0x70] = "LWin", [0x71] = "RWin", [0x72] = "Menu",
    };

    if (key >= 0 && key < (keycode_t)(sizeof(names) / sizeof(names[0]))
        && names[key])
        return names[key];
    return "Unknown";
}

/* =========================================================================
 * 键盘事件处理（修饰键状态 + ASCII 输出）
 * ========================================================================= */

static void keyboard_handle_event(const struct keyboard_event *event)
{
    /* 更新修饰键状态 */
    if (event->type == KEY_EVENT_PRESS) {
        switch (event->key) {
        case KEY_LSHIFT:     kbd_state.lshift = true; break;
        case KEY_RSHIFT:     kbd_state.rshift = true; break;
        case KEY_LCTRL:      kbd_state.lctrl = true; break;
        case KEY_RCTRL:      kbd_state.rctrl = true; break;
        case KEY_LALT:       kbd_state.lalt = true; break;
        case KEY_RALT:       kbd_state.ralt = true; break;
        case KEY_CAPSLOCK:   kbd_state.capslock = !kbd_state.capslock; break;
        case KEY_NUMLOCK:    kbd_state.numlock = !kbd_state.numlock; break;
        case KEY_SCROLLLOCK: kbd_state.scrolllock = !kbd_state.scrolllock; break;
        default: break;
        }
    } else {
        switch (event->key) {
        case KEY_LSHIFT:     kbd_state.lshift = false; break;
        case KEY_RSHIFT:     kbd_state.rshift = false; break;
        case KEY_LCTRL:      kbd_state.lctrl = false; break;
        case KEY_RCTRL:      kbd_state.rctrl = false; break;
        case KEY_LALT:       kbd_state.lalt = false; break;
        case KEY_RALT:       kbd_state.ralt = false; break;
        default: break;
        }
    }

    /* 转换为 ASCII 并输出 */
    char c = keyboard_getchar(event);
    if (c)
        tty_putchar(c);
}

/* =========================================================================
 * Scancode Set 1 解码状态机
 * 参考：OSDev PS/2 Keyboard - Scancode Set 1
 * ========================================================================= */

static void keyboard_process_scancode(uint8_t scancode)
{
    /* ---- 前缀字节处理 ---- */
    if (scancode == 0xE0) {
        scancode_extended = true;
        return;
    }

    if (scancode == 0xE1) {
        /* Pause 键序列 (Set 1): E1 14 77 E1 F0 14 F0 77，暂不处理 */
        scancode_extended = false;
        return;
    }

    /* ---- Scancode Set 1 释放码检测 ----
     * Set 1 释放码 = 按下码 | 0x80
     * 例如：Space 按下 = 0x39，释放 = 0xB9
     * 参考：OSDev PS/2 Keyboard - Scancode Set 1
     * 注意：不要和 Scancode Set 2 混淆（Set 2 用 0xF0 前缀） */
    struct keyboard_event event;
    event.extended = scancode_extended;

    if (scancode & 0x80) {
        /* 释放码：清除 bit 7 得到原始键码 */
        event.key  = (keycode_t)(scancode & 0x7F);
        event.type = KEY_EVENT_RELEASE;
    } else {
        /* 按下码 */
        event.key  = (keycode_t)scancode;
        event.type = KEY_EVENT_PRESS;
    }

    keyboard_handle_event(&event);
    scancode_extended = false;
}

/* =========================================================================
 * IRQ 处理与初始化
 * ========================================================================= */

void keyboard_irq_handler(uint8_t int_no)
{
    (void)int_no;

    /* 不断读取直到 PS/2 输出缓冲区为空
     * 参考：OSDev PS/2 Keyboard - The Keyboard ISR */
    while (inb(PS2_STATUS_PORT) & 1) {
        uint8_t scancode = inb(PS2_DATA_PORT);
        keyboard_process_scancode(scancode);
    }
}

void keyboard_init(void)
{
    /* 清零状态 */
    scancode_extended = false;
    kbd_state.lshift = kbd_state.rshift = false;
    kbd_state.lctrl = kbd_state.rctrl = false;
    kbd_state.lalt = kbd_state.ralt = false;
    kbd_state.capslock = kbd_state.numlock = false;
    kbd_state.scrolllock = false;

    /* 配置 IOAPIC: ISA IRQ1 (键盘) -> 向量 IRQ_KEYBOARD
     * 参考：OSDev IOAPIC, ACPI Spec - MADT Interrupt Source Override */
    uint32_t gsi;
    uint16_t isa_flags;
    acpi_get_isa_override(1, &gsi, &isa_flags);
    ioapic_set_redirection((uint8_t)gsi, IRQ_KEYBOARD, false, isa_flags);

    tty_print("[OK] PS/2 Keyboard initialized (IRQ1 -> GSI=");
    tty_print_dec((uint32_t)gsi);
    tty_print(" vector=");
    tty_print_dec(IRQ_KEYBOARD);
    if (gsi != 1 || isa_flags) {
        tty_print(" [override: flags=0x");
        tty_print_hex64(isa_flags);
        tty_print("]");
    }
    tty_print(")\n");
}
