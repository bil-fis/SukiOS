/* =============================================================================
 * SukiOS - PS/2 键盘驱动实现
 *
 * 参考：
 *   - OSDev PS/2 Keyboard (https://wiki.osdev.org/PS/2_Keyboard)
 *   - OSDev 8042 PS/2 Controller (https://wiki.osdev.org/%228042%22_PS/2_Controller)
 *
 * 架构概览：
 *   ┌─────────────┐    IRQ1     ┌──────────────┐    Set 1       ┌───────────────┐
 *   │ PS/2 键盘   │ ─────────→ │ 8042 控制器   │ ────────────→ │ keyboard_irq  │
 *   │ (Scancode 2)│            │ (翻译为 Set1) │               │ _handler()    │
 *   └─────────────┘            └──────────────┘               └───────┬───────┘
 *                                                                       │
 *                                                                ┌──────▼───────┐
 *                                                                │ 状态机解码    │
 *                                                                │ E0/E1/普通    │
 *                                                                └──────┬───────┘
 *                                                                       │
 *                                                   ┌───────────────────┤
 *                                                   │                   │
 *                                            ┌──────▼──────┐    ┌─────▼──────┐
 *                                            │ Ring Buffer │    │ LED 更新    │
 *                                            │ (消费者读取) │    │ 重复定时器  │
 *                                            └─────────────┘    └────────────┘
 *
 * 8042 初始化流程（参考 OSDev 8042 "Initialising the PS/2 Controller"）：
 *   Step 3: 禁用设备 (0xAD, 0xA7)
 *   Step 4: 刷新输出缓冲区
 *   Step 5: 设置配置字节 (0x20 读取, 0x60 写入)
 *   Step 6: 控制器自检 (0xAA → 0x55)
 *   Step 8: 端口 1 测试 (0xAB → 0x00)
 *   Step 9: 使能设备 (0xAE)
 *   Step 10: 键盘设备复位 (0xFF → 0xFA, 0xAA)
 *
 * PS/2 键盘命令（参考 OSDev PS/2 Keyboard "Commands"）：
 *   0xFF Reset/Self-test      0xF0 Set Scancode Set
 *   0xF4 Enable Scanning      0xF5 Disable Scanning
 *   0xF3 Set Typematic Rate   0xED Set LEDs
 *   0xEE Echo                 0xF2 Identify
 *   0xFE Resend
 *
 * PS/2 应答字节（参考 OSDev PS/2 Keyboard "Special Bytes"）：
 *   0xFA ACK                  0xFE Resend
 *   0xAA Self-test passed     0xFC/0xFD Self-test failed
 *   0x00/0xFF Key error/buffer overrun
 * ============================================================================= */

#include "kernel/keyboard.h"
#include "kernel/apic.h"
#include "kernel/acpi.h"
#include "kernel/tty.h"
#include "kernel/io.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "kernel/proc.h"

/* =============================================================================
 * PS/2 控制器端口和命令
 * 参考：OSDev 8042 PS/2 Controller - PS/2 Controller IO Ports
 * ============================================================================= */

#define PS2_DATA_PORT   0x60    /* 数据端口（读/写设备数据） */
#define PS2_CMD_PORT    0x64    /* 命令端口（写控制器命令）/ 状态端口（读控制器状态） */

/* ---- PS/2 状态寄存器位（读 0x64） ---- */
#define PS2_STATUS_OUTBUF   0x01    /* bit 0: 输出缓冲区满（有数据可读） */
#define PS2_STATUS_INBUF    0x02    /* bit 1: 输入缓冲区满（等待发送完成） */
#define PS2_STATUS_SYSFLAG  0x04    /* bit 2: 系统标志（POST 通过） */
#define PS2_STATUS_CMDDATA  0x08    /* bit 3: 0=数据给设备, 1=命令给控制器 */
#define PS2_STATUS_TIMEOUT  0x40    /* bit 6: 超时错误 */
#define PS2_STATUS_PARITY   0x80    /* bit 7: 奇偶校验错误 */

/* ---- PS/2 控制器命令 ---- */
#define PS2_CMD_READ_CFG        0x20    /* 读取配置字节 */
#define PS2_CMD_WRITE_CFG       0x60    /* 写入配置字节（下一个字节写到 0x60） */
#define PS2_CMD_DISABLE_PORT1   0xAD    /* 禁用第一个 PS/2 端口 */
#define PS2_CMD_ENABLE_PORT1    0xAE    /* 使能第一个 PS/2 端口 */
#define PS2_CMD_DISABLE_PORT2   0xA7    /* 禁用第二个 PS/2 端口 */
#define PS2_CMD_ENABLE_PORT2    0xA8    /* 使能第二个 PS/2 端口 */
#define PS2_CMD_TEST_CTRL       0xAA    /* 控制器自检 → 应答 0x55 */
#define PS2_CMD_TEST_PORT1      0xAB    /* 测试端口 1 → 应答 0x00 */
#define PS2_CMD_TEST_PORT2      0xA9    /* 测试端口 2 → 应答 0x00 */

/* ---- PS/2 配置字节位 ---- */
#define PS2_CFG_IRQ1        0x01    /* bit 0: 端口 1 中断使能 */
#define PS2_CFG_IRQ12       0x02    /* bit 1: 端口 2 中断使能 */
#define PS2_CFG_SYSFLAG     0x04    /* bit 2: 系统标志 */
#define PS2_CFG_CLK1        0x10    /* bit 4: 端口 1 时钟禁用 */
#define PS2_CFG_CLK2        0x20    /* bit 5: 端口 2 时钟禁用 */
#define PS2_CFG_TRANSLATE   0x40    /* bit 6: 端口 1 翻译（Set2 → Set1） */

/* ---- PS/2 键盘命令（发给键盘设备的，不是给控制器的） ---- */
#define KBD_CMD_RESET       0xFF    /* 复位并自检 → 0xFA + 0xAA */
#define KBD_CMD_SET_SCANCODE 0xF0   /* 设置 Scancode Set: 下一个字节 = Set 编号 */
#define KBD_CMD_ENABLE      0xF4    /* 使能扫描 */
#define KBD_CMD_DISABLE     0xF5    /* 禁用扫描 */
#define KBD_CMD_SET_RATE    0xF3    /* 设置重复速率: 下一个字节 = rate/delay */
#define KBD_CMD_SET_LEDS    0xED    /* 设置 LED: 下一个字节 = LED 状态 */
#define KBD_CMD_ECHO        0xEE    /* 回显命令 */
#define KBD_CMD_IDENTIFY    0xF2    /* 识别设备 */

/* ---- PS/2 应答字节 ---- */
#define PS2_RESP_ACK        0xFA    /* 命令已接受 */
#define PS2_RESP_RESEND     0xFE    /* 请求重发 */
#define PS2_RESP_BAT_OK     0xAA    /* 自检通过 */
#define PS2_RESP_BAT_FAIL   0xFC    /* 自检失败 */

/* ---- PS/2 超时 ---- */
#define PS2_TIMEOUT_US      100000  /* 超时：100ms（以微秒计数循环估算） */
#define PS2_TIMEOUT_DATA    100000  /* 数据超时 */
#define PS2_RETRIES         3       /* 最大重试次数 */

/* =============================================================================
 * Scancode Set 1 → 键码映射表
 * 参考：OSDev PS/2 Keyboard - Scan Code Set 1
 *
 * 8042 控制器默认开启翻译模式（bit 6 = 1），将键盘发出的 Set 2 scancode
 * 翻译为 Set 1 格式。Set 1 释放码 = 按下码 | 0x80。
 * ============================================================================= */

/* 无 E0 前缀的 Set 1 按下码 → 键码映射 */
static const keycode_t set1_make_to_keycode[128] = {
    /* 0x00 */ KEY_NONE,
    /* 0x01 Esc */ KEY_ESCAPE,
    /* 0x02-0x0D: 数字行 */
    [0x02] = KEY_1,   [0x03] = KEY_2,   [0x04] = KEY_3,
    [0x05] = KEY_4,   [0x06] = KEY_5,   [0x07] = KEY_6,
    [0x08] = KEY_7,   [0x09] = KEY_8,   [0x0A] = KEY_9,
    [0x0B] = KEY_0,   [0x0C] = KEY_MINUS, [0x0D] = KEY_EQUALS,
    /* 0x0E Bksp */ [0x0E] = KEY_BACKSPACE,
    /* 0x0F Tab */  [0x0F] = KEY_TAB,
    /* 0x10-0x19: QWERTY 行 */
    [0x10] = KEY_Q,   [0x11] = KEY_W,   [0x12] = KEY_E,
    [0x13] = KEY_R,   [0x14] = KEY_T,   [0x15] = KEY_Y,
    [0x16] = KEY_U,   [0x17] = KEY_I,   [0x18] = KEY_O,
    [0x19] = KEY_P,   [0x1A] = KEY_LBRACKET, [0x1B] = KEY_RBRACKET,
    /* 0x1C Enter */ [0x1C] = KEY_ENTER,
    /* 0x1D LCtrl */ [0x1D] = KEY_LCTRL,
    /* 0x1E-0x26: ASDF 行 */
    [0x1E] = KEY_A,   [0x1F] = KEY_S,   [0x20] = KEY_D,
    [0x21] = KEY_F,   [0x22] = KEY_G,   [0x23] = KEY_H,
    [0x24] = KEY_J,   [0x25] = KEY_K,   [0x26] = KEY_L,
    [0x27] = KEY_SEMICOLON, [0x28] = KEY_APOSTROPHE,
    /* 0x29 ` */    [0x29] = KEY_GRAVE,
    /* 0x2A LShift */ [0x2A] = KEY_LSHIFT,
    /* 0x2B \ */    [0x2B] = KEY_BACKSLASH,
    /* 0x2C-0x32: ZXCV 行 */
    [0x2C] = KEY_Z,   [0x2D] = KEY_X,   [0x2E] = KEY_C,
    [0x2F] = KEY_V,   [0x30] = KEY_B,   [0x31] = KEY_N,
    [0x32] = KEY_M,   [0x33] = KEY_COMMA, [0x34] = KEY_DOT,
    [0x35] = KEY_SLASH,
    /* 0x36 RShift */ [0x36] = KEY_RSHIFT,
    /* 0x37 KP* */   [0x37] = KEY_KP_MULTIPLY,
    /* 0x38 LAlt */  [0x38] = KEY_LALT,
    /* 0x39 Space */ [0x39] = KEY_SPACE,
    /* 0x3A CapsLock */ [0x3A] = KEY_CAPSLOCK,
    /* 0x3B-0x44 F1-F10 */
    [0x3B] = KEY_F1,  [0x3C] = KEY_F2,  [0x3D] = KEY_F3,
    [0x3E] = KEY_F4,  [0x3F] = KEY_F5,  [0x40] = KEY_F6,
    [0x41] = KEY_F7,  [0x42] = KEY_F8,  [0x43] = KEY_F9,
    [0x44] = KEY_F10,
    /* 0x45 NumLock */ [0x45] = KEY_NUMLOCK,
    /* 0x46 ScrollLock */ [0x46] = KEY_SCROLLLOCK,
    /* 0x47-0x53: 小键盘 */
    [0x47] = KEY_KP_7, [0x48] = KEY_KP_8, [0x49] = KEY_KP_9,
    [0x4A] = KEY_KP_MINUS, [0x4B] = KEY_KP_4, [0x4C] = KEY_KP_5,
    [0x4D] = KEY_KP_6, [0x4E] = KEY_KP_PLUS, [0x4F] = KEY_KP_1,
    [0x50] = KEY_KP_2, [0x51] = KEY_KP_3, [0x52] = KEY_KP_0,
    [0x53] = KEY_KP_DOT,
    /* 0x57 F11 */  [0x57] = KEY_F11,
    /* 0x58 F12 */  [0x58] = KEY_F12,
};

/* E0 扩展的 Set 1 按下码 → 键码映射
 * 参考：OSDev PS/2 Keyboard - Scancode Set 1 中 E0 开头的条目
 * 注意：E0 后面的 scancode 不能直接当键码使用！例如：
 *   E0 1D = Right Ctrl（不是 LCtrl 0x1D）
 *   E0 38 = Right Alt（不是 LAlt 0x38）
 *   E0 47 = Home（不是 KP7 0x47） */
static const keycode_t e0_make_to_keycode[128] = {
    [0x10] = KEY_NONE,           /* 多媒体: 上一曲 */
    [0x19] = KEY_NONE,           /* 多媒体: 下一曲 */
    [0x1C] = KEY_KP_ENTER,       /* 小键盘 Enter */
    [0x1D] = KEY_RCTRL,          /* Right Ctrl */
    [0x20] = KEY_NONE,           /* 多媒体: 静音 */
    [0x21] = KEY_NONE,           /* 多媒体: 计算器 */
    [0x22] = KEY_NONE,           /* 多媒体: 播放 */
    [0x24] = KEY_NONE,           /* 多媒体: 停止 */
    [0x2E] = KEY_NONE,           /* 多媒体: 音量减 */
    [0x30] = KEY_NONE,           /* 多媒体: 音量加 */
    [0x32] = KEY_NONE,           /* 多媒体: WWW 首页 */
    [0x35] = KEY_KP_DIVIDE,      /* 小键盘 / */
    [0x38] = KEY_RALT,           /* Right Alt / AltGr */
    [0x47] = KEY_HOME,           /* Home */
    [0x48] = KEY_UP,             /* Up */
    [0x49] = KEY_PAGEUP,         /* Page Up */
    [0x4B] = KEY_LEFT,           /* Left */
    [0x4D] = KEY_RIGHT,          /* Right */
    [0x4F] = KEY_END,            /* End */
    [0x50] = KEY_DOWN,           /* Down */
    [0x51] = KEY_PAGEDOWN,       /* Page Down */
    [0x52] = KEY_INSERT,         /* Insert */
    [0x53] = KEY_DELETE,         /* Delete */
    [0x5B] = KEY_LWIN,           /* Left Windows */
    [0x5C] = KEY_RWIN,           /* Right Windows */
    [0x5D] = KEY_MENU,           /* Menu / Apps */
};

/* =============================================================================
 * US QWERTY ASCII 映射表
 * 使用显式索引 [index] = value 语法
 * ============================================================================= */

/* 无 Shift 的 ASCII 映射（按键码索引） */
static const char keycode_to_ascii[128] = {
    [KEY_ESCAPE]    = 0,
    [KEY_1]         = '1',  [KEY_2]  = '2',  [KEY_3]  = '3',
    [KEY_4]         = '4',  [KEY_5]  = '5',  [KEY_6]  = '6',
    [KEY_7]         = '7',  [KEY_8]  = '8',  [KEY_9]  = '9',
    [KEY_0]         = '0',  [KEY_MINUS] = '-', [KEY_EQUALS] = '=',
    [KEY_BACKSPACE] = '\b', [KEY_TAB] = '\t',
    [KEY_Q]         = 'q',  [KEY_W]  = 'w',  [KEY_E]  = 'e',
    [KEY_R]         = 'r',  [KEY_T]  = 't',  [KEY_Y]  = 'y',
    [KEY_U]         = 'u',  [KEY_I]  = 'i',  [KEY_O]  = 'o',
    [KEY_P]         = 'p',  [KEY_LBRACKET] = '[', [KEY_RBRACKET] = ']',
    [KEY_ENTER]     = '\n',
    [KEY_A]         = 'a',  [KEY_S]  = 's',  [KEY_D]  = 'd',
    [KEY_F]         = 'f',  [KEY_G]  = 'g',  [KEY_H]  = 'h',
    [KEY_J]         = 'j',  [KEY_K]  = 'k',  [KEY_L]  = 'l',
    [KEY_SEMICOLON] = ';',  [KEY_APOSTROPHE] = '\'',
    [KEY_GRAVE]     = '`',
    [KEY_BACKSLASH] = '\\',
    [KEY_Z]         = 'z',  [KEY_X]  = 'x',  [KEY_C]  = 'c',
    [KEY_V]         = 'v',  [KEY_B]  = 'b',  [KEY_N]  = 'n',
    [KEY_M]         = 'm',  [KEY_COMMA] = ',', [KEY_DOT] = '.',
    [KEY_SLASH]     = '/',
    [KEY_SPACE]     = ' ',
    [KEY_KP_7]      = '7',  [KEY_KP_8] = '8',  [KEY_KP_9] = '9',
    [KEY_KP_MINUS]  = '-',  [KEY_KP_4] = '4',  [KEY_KP_5] = '5',
    [KEY_KP_6]      = '6',  [KEY_KP_PLUS] = '+', [KEY_KP_1] = '1',
    [KEY_KP_2]      = '2',  [KEY_KP_3] = '3',  [KEY_KP_0] = '0',
    [KEY_KP_DOT]    = '.',
};

/* Shift 修饰的 ASCII 映射（按键码索引） */
static const char keycode_to_ascii_shift[128] = {
    [KEY_1]         = '!',  [KEY_2]  = '@',  [KEY_3]  = '#',
    [KEY_4]         = '$',  [KEY_5]  = '%',  [KEY_6]  = '^',
    [KEY_7]         = '&',  [KEY_8]  = '*',  [KEY_9]  = '(',
    [KEY_0]         = ')',  [KEY_MINUS] = '_', [KEY_EQUALS] = '+',
    [KEY_Q]         = 'Q',  [KEY_W]  = 'W',  [KEY_E]  = 'E',
    [KEY_R]         = 'R',  [KEY_T]  = 'T',  [KEY_Y]  = 'Y',
    [KEY_U]         = 'U',  [KEY_I]  = 'I',  [KEY_O]  = 'O',
    [KEY_P]         = 'P',  [KEY_LBRACKET] = '{', [KEY_RBRACKET] = '}',
    [KEY_A]         = 'A',  [KEY_S]  = 'S',  [KEY_D]  = 'D',
    [KEY_F]         = 'F',  [KEY_G]  = 'G',  [KEY_H]  = 'H',
    [KEY_J]         = 'J',  [KEY_K]  = 'K',  [KEY_L]  = 'L',
    [KEY_SEMICOLON] = ':',  [KEY_APOSTROPHE] = '"',
    [KEY_GRAVE]     = '~',  [KEY_BACKSLASH] = '|',
    [KEY_Z]         = 'Z',  [KEY_X]  = 'X',  [KEY_C]  = 'C',
    [KEY_V]         = 'V',  [KEY_B]  = 'B',  [KEY_N]  = 'N',
    [KEY_M]         = 'M',  [KEY_COMMA] = '<', [KEY_DOT] = '>',
    [KEY_SLASH]     = '?',  [KEY_SPACE] = ' ',
    [KEY_KP_7]      = '7',  [KEY_KP_8] = '8',  [KEY_KP_9] = '9',
    [KEY_KP_MINUS]  = '-',  [KEY_KP_4] = '4',  [KEY_KP_5] = '5',
    [KEY_KP_6]      = '6',  [KEY_KP_PLUS] = '+', [KEY_KP_1] = '1',
    [KEY_KP_2]      = '2',  [KEY_KP_3] = '3',  [KEY_KP_0] = '0',
    [KEY_KP_DOT]    = '.',
};

/* 判断键码是否为字母键 (A-Z) */
static inline bool is_letter_key(keycode_t key)
{
    /* QWERTY 行: Q-P (0x10-0x19), ASDF 行: A-L (0x1E-0x26), ZXCV 行: Z-M (0x2C-0x32) */
    if (key >= KEY_Q && key <= KEY_P) return true;
    if (key >= KEY_A && key <= KEY_L) return true;
    if (key >= KEY_Z && key <= KEY_M) return true;
    return false;
}

/* 判断键码是否为小键盘数字键 */
static inline bool is_kp_number(keycode_t key)
{
    return (key >= KEY_KP_7 && key <= KEY_KP_9) ||
           (key >= KEY_KP_4 && key <= KEY_KP_6) ||
           (key >= KEY_KP_1 && key <= KEY_KP_3) ||
           key == KEY_KP_0 || key == KEY_KP_DOT;
}

/* 判断键码是否为小键盘操作键（+、-、*、/、Enter） */
static inline bool is_kp_operator(keycode_t key)
{
    return key == KEY_KP_PLUS || key == KEY_KP_MINUS ||
           key == KEY_KP_MULTIPLY || key == KEY_KP_DIVIDE ||
           key == KEY_KP_ENTER;
}

/* =============================================================================
 * 环形缓冲区
 * 参考：OSDev PS/2 Keyboard - Driver Model (Command Queue)
 * ============================================================================= */

#define KBD_BUFFER_SIZE 256

struct kbd_ring_buffer {
    char data[KBD_BUFFER_SIZE];
    volatile uint16_t head;   /* 生产者写入位置 */
    volatile uint16_t tail;   /* 消费者读取位置 */
};

static struct kbd_ring_buffer kbd_buffer;

static bool ring_buffer_is_empty(const struct kbd_ring_buffer *rb)
{
    return rb->head == rb->tail;
}

static bool ring_buffer_is_full(const struct kbd_ring_buffer *rb)
{
    return (rb->head + 1) % KBD_BUFFER_SIZE == rb->tail;
}

static void ring_buffer_push(struct kbd_ring_buffer *rb, char c)
{
    if (ring_buffer_is_full(rb))
        return;  /* 丢弃溢出数据 */
    rb->data[rb->head] = c;
    rb->head = (rb->head + 1) % KBD_BUFFER_SIZE;
}

static char ring_buffer_pop(struct kbd_ring_buffer *rb)
{
    if (ring_buffer_is_empty(rb))
        return 0;
    char c = rb->data[rb->tail];
    rb->tail = (rb->tail + 1) % KBD_BUFFER_SIZE;
    return c;
}

/* =============================================================================
 * 全局状态
 * ============================================================================= */

static struct keyboard_state kbd_state;

/* Scancode 解码状态机 */
enum scancode_state {
    STATE_NORMAL,               /* 正常状态 */
    STATE_EXPECT_E0_DATA,       /* 收到 E0，等待下一个字节 */
    STATE_EXPECT_E1_DATA_1,     /* 收到 E1，等待第 1 个数据字节 (Pause 键序列) */
    STATE_EXPECT_E1_DATA_2,     /* E1 序列第 2 字节，等待第 3 个 */
    STATE_EXPECT_E1_RELEASE_1,  /* E1 释放序列 */
    STATE_EXPECT_E1_RELEASE_2,  /* E1 释放序列 */
    STATE_EXPECT_E0_RELEASE,    /* E0 PrintScreen 释放序列 */
};

static volatile enum scancode_state scancode_st = STATE_NORMAL;
static uint8_t e0_saved_data = 0;  /* E0 PrintScreen 第一个字节 */

/* 按键重复（Key Repeat）状态 */
static keycode_t    repeat_key = KEY_NONE;
static uint16_t     repeat_delay_ms = 500;    /* 首次重复延迟 */
static uint16_t     repeat_rate_ms  = 33;     /* 重复间隔 (~30Hz) */
static uint32_t     repeat_timer = 0;         /* 已持续的 tick 数 */

/* LED 缓存（减少发送次数） */
static uint8_t led_cache = 0;

/* =============================================================================
 * PS/2 控制器辅助函数
 * 参考：OSDev 8042 PS/2 Controller - Sending Bytes To Device/s
 * ============================================================================= */

/* 等待输入缓冲区清空（可写入） */
static bool ps2_wait_write(uint32_t timeout)
{
    while (timeout--) {
        if (!(inb(PS2_CMD_PORT) & PS2_STATUS_INBUF))
            return true;
        io_wait();
    }
    return false;
}

/* 等待输出缓冲区有数据（可读取） */
static bool ps2_wait_read(uint32_t timeout)
{
    while (timeout--) {
        if (inb(PS2_CMD_PORT) & PS2_STATUS_OUTBUF)
            return true;
        io_wait();
    }
    return false;
}

/* 发送控制器命令（写到 0x64） */
static bool ps2_send_controller_cmd(uint8_t cmd)
{
    if (!ps2_wait_write(PS2_TIMEOUT_US))
        return false;
    outb(PS2_CMD_PORT, cmd);
    return true;
}

/* 发送数据到 PS/2 设备端口 1（写到 0x60） */
static bool ps2_send_device(uint8_t data)
{
    if (!ps2_wait_write(PS2_TIMEOUT_US))
        return false;
    outb(PS2_DATA_PORT, data);
    return true;
}

/* 从 PS/2 数据端口读取一个字节（带超时） */
static bool ps2_read_data(uint8_t *data)
{
    if (!ps2_wait_read(PS2_TIMEOUT_DATA))
        return false;
    *data = inb(PS2_DATA_PORT);
    return true;
}

/* 刷新输出缓冲区（丢弃所有待读数据） */
static void ps2_flush(void)
{
    uint8_t dummy;
    while (inb(PS2_CMD_PORT) & PS2_STATUS_OUTBUF)
        dummy = inb(PS2_DATA_PORT);
    (void)dummy;
}

/* =============================================================================
 * PS/2 键盘命令（参考 OSDev PS/2 Keyboard "Commands"）
 * 发送命令并等待 ACK，支持重试
 * ============================================================================= */

/* 发送键盘命令并等待 ACK
 * @cmd: 键盘命令字节
 * @return: true=成功收到 ACK, false=超时或重试耗尽 */
static bool kbd_send_command(uint8_t cmd)
{
    for (int retry = 0; retry < PS2_RETRIES; retry++) {
        /* 发送命令 */
        if (!ps2_send_device(cmd))
            return false;

        /* 等待应答 */
        uint8_t resp;
        if (!ps2_read_data(&resp))
            return false;

        if (resp == PS2_RESP_ACK)
            return true;
        if (resp == PS2_RESP_RESEND)
            continue;  /* 重试 */
        /* 其他应答（0xFC/0xFD 自检失败等）视为失败 */
        return false;
    }
    return false;
}

/* 发送带参数的键盘命令（如 0xF0 xx, 0xF3 xx, 0xED xx）
 * @cmd: 命令字节
 * @param: 参数字节
 * @return: true=成功 */
static bool kbd_send_command_with_param(uint8_t cmd, uint8_t param)
{
    for (int retry = 0; retry < PS2_RETRIES; retry++) {
        /* 发送命令 */
        if (!ps2_send_device(cmd))
            return false;

        /* 等待命令 ACK */
        uint8_t resp;
        if (!ps2_read_data(&resp))
            return false;

        if (resp == PS2_RESP_RESEND)
            continue;
        if (resp != PS2_RESP_ACK)
            return false;

        /* 发送参数 */
        if (!ps2_send_device(param))
            return false;

        /* 等待参数 ACK */
        if (!ps2_read_data(&resp))
            return false;

        if (resp == PS2_RESP_RESEND)
            continue;
        if (resp == PS2_RESP_ACK)
            return true;
        return false;
    }
    return false;
}

/* =============================================================================
 * LED 控制（参考 OSDev PS/2 Keyboard - LED states）
 *
 * LED 状态字节:
 *   bit 0: ScrollLock
 *   bit 1: NumLock
 *   bit 2: CapsLock
 * ============================================================================= */

static void kbd_update_leds(void)
{
    uint8_t led_state = 0;
    if (kbd_state.scrolllock) led_state |= 0x01;
    if (kbd_state.numlock)    led_state |= 0x02;
    if (kbd_state.capslock)   led_state |= 0x04;

    /* 仅在实际变化时发送，减少总线通信 */
    if (led_state == led_cache)
        return;

    if (kbd_send_command_with_param(KBD_CMD_SET_LEDS, led_state))
        led_cache = led_state;
}

/* =============================================================================
 * Typematic Rate 设置（参考 OSDev PS/2 Keyboard - Set typematic rate and delay）
 *
 * Typematic 字节:
 *   bits 0-4: 重复速率 (00000b=30Hz 最慢, 11111b=~2Hz 最快)
 *              实际常用值: 0b10100 = 10.9 chars/s
 *   bits 5-6: 延迟 (00b=250ms, 01b=500ms, 10b=750ms, 11b=1000ms)
 *   bit 7:   必须为 0
 * ============================================================================= */

static void kbd_set_typematic(void)
{
    /* 重复速率 ~10.9 chars/s (0b10100), 延迟 500ms (0b01) */
    uint8_t rate_byte = 0x20 | 0x00 | 0x00;
    /* bits 5-6 = 01 (500ms), bits 0-4 = 00000 (30Hz 默认) */
    rate_byte = (1 << 5);  /* 500ms 延迟, 30Hz 重复 */
    kbd_send_command_with_param(KBD_CMD_SET_RATE, rate_byte);
}

/* =============================================================================
 * ASCII 转换
 * ============================================================================= */

static char keycode_to_ascii_char(keycode_t key)
{
    bool shift = kbd_state.lshift || kbd_state.rshift;

    /* 字母键：CapsLock XOR Shift */
    if (is_letter_key(key)) {
        char base = keycode_to_ascii[key];
        if (!base) return 0;
        if (kbd_state.capslock ^ shift)
            return base - 32;  /* 转大写 */
        return base;           /* 小写 */
    }

    /* 小键盘数字：NumLock 时产生数字，否则不产生 ASCII */
    if (is_kp_number(key)) {
        if (kbd_state.numlock)
            return keycode_to_ascii[key];
        return 0;
    }

    /* 小键盘操作符（+、-、*、/）始终产生 ASCII */
    if (is_kp_operator(key))
        return keycode_to_ascii[key];

    /* 其他可打印键：Shift 切换 */
    if (shift) {
        char c = keycode_to_ascii_shift[key];
        if (c) return c;
    }
    return keycode_to_ascii[key];
}

/* =============================================================================
 * 键盘事件处理
 * ============================================================================= */

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
        case KEY_CAPSLOCK:
            kbd_state.capslock = !kbd_state.capslock;
            kbd_update_leds();
            break;
        case KEY_NUMLOCK:
            kbd_state.numlock = !kbd_state.numlock;
            kbd_update_leds();
            break;
        case KEY_SCROLLLOCK:
            kbd_state.scrolllock = !kbd_state.scrolllock;
            kbd_update_leds();
            break;
        default:
            break;
        }

        /* 转换为 ASCII 并存入缓冲区 */
        char c = keycode_to_ascii_char(event->key);
        if (c)
            ring_buffer_push(&kbd_buffer, c);

        /* 更新按键重复状态 */
        repeat_key = event->key;
        repeat_timer = 0;

    } else {  /* KEY_EVENT_RELEASE */
        switch (event->key) {
        case KEY_LSHIFT:     kbd_state.lshift = false; break;
        case KEY_RSHIFT:     kbd_state.rshift = false; break;
        case KEY_LCTRL:      kbd_state.lctrl = false; break;
        case KEY_RCTRL:      kbd_state.rctrl = false; break;
        case KEY_LALT:       kbd_state.lalt = false; break;
        case KEY_RALT:       kbd_state.ralt = false; break;
        default:
            break;
        }

        /* 释放键时停止重复 */
        if (event->key == repeat_key) {
            repeat_key = KEY_NONE;
            repeat_timer = 0;
        }
    }
}

/* =============================================================================
 * Scancode Set 1 解码状态机
 *
 * 参考 OSDev PS/2 Keyboard - Scan Code Set 1
 *
 * Set 1 格式：
 *   - 普通键按下: 1 字节 (0x01-0x58, 不含 E0/E1)
 *   - 普通键释放: 1 字节 = 按下码 | 0x80
 *   - E0 扩展键按下: E0 + 1 字节
 *   - E0 扩展键释放: E0 + (按下码 | 0x80)
 *   - PrintScreen 按下: E0, 0x2A, E0, 0x37 (4 字节序列)
 *   - PrintScreen 释放: E0, 0xB7, E0, 0xAA (4 字节序列)
 *   - Pause 按下: E1, 0x1D, 0x45, E1, 0x9D, 0xC5 (6 字节序列)
 *   - Pause 无释放码
 * ============================================================================= */

void keyboard_process_scancode(uint8_t scancode)
{
    struct keyboard_event event;

    switch (scancode_st) {
    case STATE_NORMAL:
        if (scancode == 0xE0) {
            scancode_st = STATE_EXPECT_E0_DATA;
            return;
        }
        if (scancode == 0xE1) {
            scancode_st = STATE_EXPECT_E1_DATA_1;
            return;
        }

        /* 普通键 */
        event.extended = false;
        if (scancode & 0x80) {
            event.key  = set1_make_to_keycode[scancode & 0x7F];
            event.type = KEY_EVENT_RELEASE;
        } else {
            event.key  = set1_make_to_keycode[scancode];
            event.type = KEY_EVENT_PRESS;
        }
        keyboard_handle_event(&event);
        break;

    case STATE_EXPECT_E0_DATA:
        scancode_st = STATE_NORMAL;
        event.extended = true;

        if (scancode & 0x80) {
            /* E0 扩展键释放 */
            event.key  = e0_make_to_keycode[scancode & 0x7F];
            event.type = KEY_EVENT_RELEASE;
            keyboard_handle_event(&event);
        } else if (scancode == 0x2A) {
            /* PrintScreen 按下序列: E0 2A E0 37
             * 保存第一个字节，等待后续序列 */
            scancode_st = STATE_EXPECT_E0_RELEASE;
            e0_saved_data = scancode;
        } else {
            /* E0 扩展键按下 */
            event.key  = e0_make_to_keycode[scancode];
            event.type = KEY_EVENT_PRESS;
            keyboard_handle_event(&event);
        }
        break;

    case STATE_EXPECT_E0_RELEASE:
        /* PrintScreen 按下: E0 2A E0 37
         * 或 PrintScreen 释放: E0 B7 E0 AA */
        scancode_st = STATE_NORMAL;
        event.extended = true;
        event.key  = KEY_PRINTSCREEN;
        event.type = KEY_EVENT_PRESS;
        keyboard_handle_event(&event);
        break;

    case STATE_EXPECT_E1_DATA_1:
        /* Pause 序列: E1 1D 45 E1 9D C5
         * 第 1 字节 = 0x1D, 直接跳到第 2 字节 */
        scancode_st = STATE_EXPECT_E1_DATA_2;
        break;

    case STATE_EXPECT_E1_DATA_2:
        /* 第 2 字节 = 0x45 */
        scancode_st = STATE_EXPECT_E1_RELEASE_1;
        break;

    case STATE_EXPECT_E1_RELEASE_1:
        /* 释放第 1 字节 = E1 */
        if (scancode == 0xE1) {
            scancode_st = STATE_EXPECT_E1_RELEASE_2;
        } else {
            scancode_st = STATE_NORMAL;  /* 序列异常，重置 */
        }
        break;

    case STATE_EXPECT_E1_RELEASE_2:
        /* 释放第 2 字节 = 0x9D */
        scancode_st = STATE_NORMAL;
        /* Pause 键按下事件（无释放码） */
        event.extended = true;
        event.key  = KEY_PAUSE;
        event.type = KEY_EVENT_PRESS;
        keyboard_handle_event(&event);
        break;
    }
}

/* =============================================================================
 * 按键重复（Key Repeat）
 *
 * 由定时器中断驱动（keyboard_timer_tick），每个 tick 约为 10ms。
 * 长按按键时先等待 delay_ticks，然后以 rate_ticks 间隔重复产生字符。
 * ============================================================================= */

#define TICKS_PER_MS     10      /* 假设 timer 为 ~100Hz → 1 tick ≈ 10ms */
#define REPEAT_DELAY_TICKS   (repeat_delay_ms / TICKS_PER_MS)
#define REPEAT_RATE_TICKS    (repeat_rate_ms / TICKS_PER_MS)

void keyboard_timer_tick(void)
{
    if (repeat_key == KEY_NONE)
        return;

    repeat_timer++;

    /* 延迟阶段：等待首次重复 */
    if (repeat_timer < REPEAT_DELAY_TICKS)
        return;

    /* 首次重复后，以 rate 间隔重复 */
    uint32_t elapsed = repeat_timer - REPEAT_DELAY_TICKS;
    if (elapsed > 0 && (elapsed % REPEAT_RATE_TICKS) == 0) {
        char c = keycode_to_ascii_char(repeat_key);
        if (c)
            ring_buffer_push(&kbd_buffer, c);
    }
}

/* =============================================================================
 * 公共接口
 * ============================================================================= */

char keyboard_getchar_nb(void)
{
    /* 先检查缓冲区中是否有待处理的字符 */
    char c = ring_buffer_pop(&kbd_buffer);
    if (c) return c;
    return 0;
}

char keyboard_getchar(void)
{
    /* 从缓冲区读取（非阻塞，由上层轮询） */
    return ring_buffer_pop(&kbd_buffer);
}

bool keyboard_peek_event(void)
{
    return !ring_buffer_is_empty(&kbd_buffer);
}

const struct keyboard_state *keyboard_get_state(void)
{
    return &kbd_state;
}

const char *keyboard_get_scancode_name(keycode_t key)
{
    static const char *names[] = {
        [KEY_NONE]       = "None",
        [KEY_ESCAPE]     = "Esc",
        [KEY_1] = "1", [KEY_2] = "2", [KEY_3] = "3",
        [KEY_4] = "4", [KEY_5] = "5", [KEY_6] = "6",
        [KEY_7] = "7", [KEY_8] = "8", [KEY_9] = "9",
        [KEY_0] = "0",
        [KEY_MINUS] = "-",   [KEY_EQUALS] = "=",   [KEY_BACKSPACE] = "Bksp",
        [KEY_TAB] = "Tab",
        [KEY_Q] = "Q", [KEY_W] = "W", [KEY_E] = "E",
        [KEY_R] = "R", [KEY_T] = "T", [KEY_Y] = "Y",
        [KEY_U] = "U", [KEY_I] = "I", [KEY_O] = "O",
        [KEY_P] = "P",
        [KEY_LBRACKET] = "[", [KEY_RBRACKET] = "]",
        [KEY_ENTER] = "Enter", [KEY_LCTRL] = "LCtrl",
        [KEY_A] = "A", [KEY_S] = "S", [KEY_D] = "D",
        [KEY_F] = "F", [KEY_G] = "G", [KEY_H] = "H",
        [KEY_J] = "J", [KEY_K] = "K", [KEY_L] = "L",
        [KEY_SEMICOLON] = ";", [KEY_APOSTROPHE] = "'", [KEY_GRAVE] = "`",
        [KEY_LSHIFT] = "LShift", [KEY_BACKSLASH] = "\\",
        [KEY_Z] = "Z", [KEY_X] = "X", [KEY_C] = "C",
        [KEY_V] = "V", [KEY_B] = "B", [KEY_N] = "N",
        [KEY_M] = "M",
        [KEY_COMMA] = ",", [KEY_DOT] = ".", [KEY_SLASH] = "/",
        [KEY_RSHIFT] = "RShift", [KEY_KP_MULTIPLY] = "KP*", [KEY_LALT] = "LAlt",
        [KEY_SPACE] = "Space", [KEY_CAPSLOCK] = "Caps",
        [KEY_F1] = "F1", [KEY_F2] = "F2", [KEY_F3] = "F3",
        [KEY_F4] = "F4", [KEY_F5] = "F5", [KEY_F6] = "F6",
        [KEY_F7] = "F7", [KEY_F8] = "F8", [KEY_F9] = "F9",
        [KEY_F10] = "F10",
        [KEY_NUMLOCK] = "NumLk", [KEY_SCROLLLOCK] = "ScrLk",
        [KEY_KP_7] = "KP7", [KEY_KP_8] = "KP8", [KEY_KP_9] = "KP9",
        [KEY_KP_MINUS] = "KP-", [KEY_KP_4] = "KP4", [KEY_KP_5] = "KP5",
        [KEY_KP_6] = "KP6", [KEY_KP_PLUS] = "KP+", [KEY_KP_1] = "KP1",
        [KEY_KP_2] = "KP2", [KEY_KP_3] = "KP3", [KEY_KP_0] = "KP0",
        [KEY_KP_DOT] = "KP.", [KEY_F11] = "F11", [KEY_F12] = "F12",
        [KEY_KP_ENTER] = "KPEnt", [KEY_RCTRL] = "RCtrl",
        [KEY_KP_DIVIDE] = "KP/", [KEY_RALT] = "RAlt",
        [KEY_PAUSE] = "Pause", [KEY_PRINTSCREEN] = "PrtSc",
        [KEY_HOME] = "Home", [KEY_UP] = "Up", [KEY_PAGEUP] = "PgUp",
        [KEY_LEFT] = "Left", [KEY_RIGHT] = "Right",
        [KEY_END] = "End", [KEY_DOWN] = "Down", [KEY_PAGEDOWN] = "PgDn",
        [KEY_INSERT] = "Ins", [KEY_DELETE] = "Del",
        [KEY_LWIN] = "LWin", [KEY_RWIN] = "RWin", [KEY_MENU] = "Menu",
    };

    if (key >= 0 && key < (keycode_t)(sizeof(names) / sizeof(names[0]))
        && names[key])
        return names[key];
    return "Unknown";
}

/* =============================================================================
 * IRQ 处理
 * ============================================================================= */

void keyboard_irq_handler(uint8_t int_no)
{
    (void)int_no;

    /* 不断读取直到 PS/2 输出缓冲区为空
     * 参考：OSDev PS/2 Keyboard - Using Interrupts */
    while (inb(PS2_CMD_PORT) & PS2_STATUS_OUTBUF) {
        uint8_t scancode = inb(PS2_DATA_PORT);
        keyboard_process_scancode(scancode);
    }
}

/* =============================================================================
 * 8042 PS/2 Controller 初始化 + 键盘设备初始化
 * 参考：OSDev 8042 "Initialising the PS/2 Controller"
 * 参考：OSDev PS/2 Keyboard "Commands"
 * ============================================================================= */

void keyboard_init(void)
{
    memset(&kbd_state, 0, sizeof(kbd_state));
    memset(&kbd_buffer, 0, sizeof(kbd_buffer));
    scancode_st = STATE_NORMAL;
    repeat_key = KEY_NONE;
    repeat_timer = 0;
    led_cache = 0xFF;  /* 强制首次 LED 更新 */

    bool init_ok = true;

    /* ---- Step 3: 禁用 PS/2 端口 ---- */
    ps2_send_controller_cmd(PS2_CMD_DISABLE_PORT1);
    ps2_send_controller_cmd(PS2_CMD_DISABLE_PORT2);
    io_wait(); io_wait();

    /* ---- Step 4: 刷新输出缓冲区 ---- */
    ps2_flush();

    /* ---- Step 5: 设置控制器配置字节 ----
     * 参考 OSDev 8042: "disable IRQs and translation for port 1 by clearing bits 0 and 6.
     *  You should also make sure the clock signal is enabled by clearing bit 4." */
    ps2_send_controller_cmd(PS2_CMD_READ_CFG);
    uint8_t cfg;
    if (!ps2_read_data(&cfg)) {
        tty_print("[WARN] PS/2: Failed to read config byte\n");
        cfg = 0;  /* 安全默认值 */
    }

    /* 清除 bit 0 (IRQ1), bit 1 (IRQ12), bit 6 (translation)
     * 保留 bit 2 (system flag), 清除 bit 4 (clock disable) */
    cfg &= ~(PS2_CFG_IRQ1 | PS2_CFG_IRQ12 | PS2_CFG_TRANSLATE | PS2_CFG_CLK1);

    ps2_send_controller_cmd(PS2_CMD_WRITE_CFG);
    ps2_send_device(cfg);
    io_wait();

    /* ---- Step 6: 控制器自检 (0xAA → 0x55) ---- */
    if (!ps2_send_controller_cmd(PS2_CMD_TEST_CTRL)) {
        tty_print("[WARN] PS/2: Controller self-test send failed\n");
    } else {
        uint8_t result = 0;
        if (!ps2_read_data(&result) || result != 0x55) {
            tty_print("[WARN] PS/2: Controller self-test failed (");
            tty_print_hex64(result);
            tty_print(")\n");
        }
    }

    /* 自检后恢复配置字节（某些硬件会被重置） */
    ps2_send_controller_cmd(PS2_CMD_WRITE_CFG);
    ps2_send_device(cfg);
    io_wait();

    /* ---- Step 8: 端口 1 测试 (0xAB → 0x00) ---- */
    if (!ps2_send_controller_cmd(PS2_CMD_TEST_PORT1)) {
        tty_print("[WARN] PS/2: Port 1 test send failed\n");
        init_ok = false;
    } else {
        uint8_t result = 0;
        if (!ps2_read_data(&result) || result != 0x00) {
            tty_print("[WARN] PS/2: Port 1 test failed (");
            tty_print_hex64(result);
            tty_print(")\n");
            init_ok = false;
        }
    }

    if (!init_ok) {
        tty_print("[FAIL] PS/2 Controller init failed, keyboard may not work\n");
        return;
    }

    /* ---- Step 9: 使能端口 1 ---- */
    ps2_send_controller_cmd(PS2_CMD_ENABLE_PORT1);
    io_wait();

    /* ---- Step 10: 键盘设备命令（IRQ1 仍关闭，防止 IRQ handler 竞争数据端口）
     * 参考 OSDev PS/2 Keyboard "Commands"
     * 注意：此时 config byte 中 IRQ1 = 0，8042 不会产生 IRQ，
     *       所有 PS/2 应答通过轮询 ps2_read_data 读取。 ---- */

    /* 键盘复位/自检 (0xFF)
     * 参考：0xFF → 0xFA (ACK) + 0xAA (BAT OK) 或 0xFC (BAT fail) */
    if (kbd_send_command(KBD_CMD_RESET)) {
        uint8_t bat;
        if (ps2_read_data(&bat)) {
            if (bat == PS2_RESP_BAT_OK) {
                tty_print("  [KBD] Self-test passed\n");
            } else {
                tty_print("  [KBD] Self-test result: 0x");
                tty_print_hex64(bat);
                tty_print("\n");
            }
        }
        ps2_flush();  /* 丢弃可能的设备 ID 字节 (0xAB 0x83) */
    } else {
        tty_print("[WARN] PS/2: Keyboard reset command failed\n");
    }

    /* 设置 Typematic Rate (0xF3)
     * 参考 OSDev: bits 0-4 = rate, bits 5-6 = delay, bit 7 = 0
     * 设置为: 500ms 延迟, 默认速率 */
    kbd_set_typematic();

    /* 使能键盘扫描 (0xF4)
     * 参考 OSDev PS/2 Keyboard: 0xF4 → 0xFA (ACK) */
    if (kbd_send_command(KBD_CMD_ENABLE)) {
        tty_print("  [KBD] Scanning enabled\n");
    } else {
        tty_print("[WARN] PS/2: Failed to enable scanning\n");
    }

    /* 更新 LED 初始状态
     * 默认 NumLock 开启（商业 OS 默认行为） */
    kbd_state.numlock = true;
    kbd_update_leds();

    /* 刷新所有积累的 scancode（命令期间用户按键产生的） */
    ps2_flush();

    /* ---- Step 11: 配置中断并使能 ----
     * 此时键盘已完全就绪（Set 2, 扫描使能, LED 已设）。
     * 启用翻译模式：8042 自动将键盘的 Set 2 scancode 翻译为 Set 1 格式。
     * 参考 OSDev: "Normally on PC compatible systems the keyboard itself uses
     *   scan code set 2 and the keyboard controller translates this into
     *   scan code set 1 for compatibility."
     * 这是业界标准做法，不依赖键盘是否支持切换到 Set 1。 */

    /* 配置 IOAPIC: ISA IRQ1 (键盘) -> 向量 IRQ_KEYBOARD
     * 参考：OSDev IOAPIC, ACPI Spec - MADT Interrupt Source Override */
    uint32_t gsi;
    uint16_t isa_flags;
    acpi_get_isa_override(1, &gsi, &isa_flags);
    ioapic_set_redirection((uint8_t)gsi, IRQ_KEYBOARD, false, isa_flags);

    /* 写入最终配置字节: IRQ1 + 翻译模式使能 */
    cfg |= PS2_CFG_IRQ1 | PS2_CFG_TRANSLATE;

    // 等待输入缓冲区空闲，发送“写配置”命令 (0x60)
    while (inb(PS2_CMD_PORT) & PS2_STATUS_INBUF);
    outb(PS2_CMD_PORT, PS2_CMD_WRITE_CFG);
    // 再次等待空闲，发送新的配置字节
    while (inb(PS2_CMD_PORT) & PS2_STATUS_INBUF);
    outb(PS2_DATA_PORT, cfg);

    tty_print("  [KBD] IRQ1 enabled (unverified, trust the write).\n");

    /* 最终刷新（使能 IRQ 后可能立即到达的 scancode） */
    ps2_flush();

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

    uint32_t low = ioapic_read(IOAPIC_REDIR_BASE + gsi * 2);
    tty_print("  [DEBUG] IOAPIC redir low: 0x");
    tty_print_hex64(low);
    tty_print("\n");
}

/**
 * 重新使能键盘中断（IOAPIC + PS/2 配置）
 * 在启动用户进程前调用，用于修复中断丢失问题。
 */
void keyboard_reenable_irq(void) {
    uint32_t gsi;
    uint16_t isa_flags;
    acpi_get_isa_override(1, &gsi, &isa_flags);

    // 1. 重新配置 IOAPIC 重定向条目，强制取消屏蔽
    ioapic_set_redirection((uint8_t)gsi, IRQ_KEYBOARD, false, isa_flags);

    // 2. 再次写入 PS/2 配置字节，确保 IRQ1 使能位绝对被设置
    uint8_t cfg;
    while (inb(PS2_CMD_PORT) & PS2_STATUS_INBUF);
    outb(PS2_CMD_PORT, PS2_CMD_READ_CFG);
    while (!(inb(PS2_CMD_PORT) & PS2_STATUS_OUTBUF));
    cfg = inb(PS2_DATA_PORT);

    cfg |= PS2_CFG_IRQ1 | PS2_CFG_TRANSLATE;
    while (inb(PS2_CMD_PORT) & PS2_STATUS_INBUF);
    outb(PS2_CMD_PORT, PS2_CMD_WRITE_CFG);
    while (inb(PS2_CMD_PORT) & PS2_STATUS_INBUF);
    outb(PS2_DATA_PORT, cfg);

    // 刷新可能积累的垃圾数据
    while (inb(PS2_CMD_PORT) & PS2_STATUS_OUTBUF)
        inb(PS2_DATA_PORT);
}

/**
 * 阻塞读取字符（带轮询后备）
 * 即使键盘 IRQ 未送达，只要按下按键就能读取。
 */
char keyboard_getchar_blocking(void) {
    char c;
    while (1) {
        // 首先检查缓冲区（由中断或轮询填充）
        c = keyboard_getchar_nb();
        if (c) return c;

        // 后备轮询：直接检查 PS/2 输出缓冲区
        // 这是参考 OSDev “轮询模式”，完全绕过中断
        uint8_t status = inb(PS2_CMD_PORT);
        while (status & PS2_STATUS_OUTBUF) {
            uint8_t scancode = inb(PS2_DATA_PORT);
            keyboard_process_scancode(scancode);
            status = inb(PS2_CMD_PORT);
        }

        // 再次检查缓冲区（刚解码的字符）
        c = keyboard_getchar_nb();
        if (c) return c;

        // 没有字符，让出 CPU，等待下次调度
//        schedule();
    }
}