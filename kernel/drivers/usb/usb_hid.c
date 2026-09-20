/*
 * kernel/drivers/usb/usb_hid.c
 * -----------------------------------------------------------------------------
 * USB HID boot 协议类驱动（键盘 / 鼠标）。
 *
 * 依据 OSDev Wiki「USB Human Input Devices」：
 *   - 键盘 boot 报告 8 字节：[修饰键][保留][键码 x6]；差异比较得按下/释放。
 *   - 鼠标 boot 报告：按键(bit0..2) + dx + dy（可选第 4 字节滚轮）。
 *
 * 为最大化复用既有输入栈（INPUT_SERVER/console 均消费 PS/2 set-1 扫描码与
 * PS/2 3/4 字节鼠标包），本驱动把 HID 事件**转译**为 PS/2 事件注入：
 *   - 键盘：HID usage -> set-1 make/break（扩展键加 0xE0 前缀）-> kbd_feed_byte。
 *   - 鼠标：合成为 PS/2 3/4 字节包 -> mouse_feed_byte（长度与当前 PS/2 模式一致）。
 * 这样 USB 键鼠与 PS/2 键鼠对上层完全同构。
 */
#include <kernel/types.h>
#include <kernel/usb_hid.h>
#include <kernel/usb.h>
#include <kernel/uhci.h>
#include <kernel/keyboard.h>
#include <kernel/mouse.h>
#include <kernel/console.h>
#include <kernel/string.h>

#define HID_E0 0xE000u

/* HID usage(0x04..0x71) -> PS/2 set-1 扫描码（含 E0 前缀标志）。 */
static const uint16_t g_hid_set1[256] = {
    [0x04] = 0x001E, [0x05] = 0x0030, [0x06] = 0x002E, [0x07] = 0x0020,
    [0x08] = 0x0012, [0x09] = 0x0021, [0x0A] = 0x0022, [0x0B] = 0x0023,
    [0x0C] = 0x0017, [0x0D] = 0x0024, [0x0E] = 0x0025, [0x0F] = 0x0026,
    [0x10] = 0x0032, [0x11] = 0x0031, [0x12] = 0x0018, [0x13] = 0x0019,
    [0x14] = 0x0010, [0x15] = 0x0013, [0x16] = 0x001F, [0x17] = 0x0014,
    [0x18] = 0x0016, [0x19] = 0x002F, [0x1A] = 0x0011, [0x1B] = 0x002D,
    [0x1C] = 0x0015, [0x1D] = 0x002C, [0x1E] = 0x0002, [0x1F] = 0x0003,
    [0x20] = 0x0004, [0x21] = 0x0005, [0x22] = 0x0006, [0x23] = 0x0007,
    [0x24] = 0x0008, [0x25] = 0x0009, [0x26] = 0x000A, [0x27] = 0x000B,
    [0x28] = 0x001C, [0x29] = 0x0001, [0x2A] = 0x000E, [0x2B] = 0x000F,
    [0x2C] = 0x0039, [0x2D] = 0x000C, [0x2E] = 0x000D, [0x2F] = 0x001A,
    [0x30] = 0x001B, [0x31] = 0x002B, [0x33] = 0x0027, [0x34] = 0x0028,
    [0x35] = 0x0029, [0x36] = 0x0033, [0x37] = 0x0034, [0x38] = 0x0035,
    [0x39] = 0x003A, /* CapsLock */
    [0x3A] = 0x003B, [0x3B] = 0x003C, [0x3C] = 0x003D, [0x3D] = 0x003E,
    [0x3E] = 0x003F, [0x3F] = 0x0040, [0x40] = 0x0041, [0x41] = 0x0042,
    [0x42] = 0x0043, [0x43] = 0x0044, [0x44] = 0x0057, [0x45] = 0x0058,
    [0x49] = (uint16_t)(HID_E0 | 0x52), [0x4A] = (uint16_t)(HID_E0 | 0x47),
    [0x4B] = (uint16_t)(HID_E0 | 0x49), [0x4C] = (uint16_t)(HID_E0 | 0x53),
    [0x4D] = (uint16_t)(HID_E0 | 0x4F), [0x4E] = (uint16_t)(HID_E0 | 0x51),
    [0x4F] = (uint16_t)(HID_E0 | 0x4D), [0x50] = (uint16_t)(HID_E0 | 0x4B),
    [0x51] = (uint16_t)(HID_E0 | 0x50), [0x52] = (uint16_t)(HID_E0 | 0x48),
    [0x53] = 0x0045, /* NumLock */
    [0x54] = (uint16_t)(HID_E0 | 0x35), [0x55] = 0x0037, [0x56] = 0x004A,
    [0x57] = 0x004E, [0x58] = (uint16_t)(HID_E0 | 0x1C),
    [0x59] = 0x004F, [0x5A] = 0x0050, [0x5B] = 0x0051, [0x5C] = 0x004B,
    [0x5D] = 0x004C, [0x5E] = 0x004D, [0x5F] = 0x0047, [0x60] = 0x0048,
    [0x61] = 0x0049, [0x62] = 0x0052, [0x63] = 0x0053,
    [0x64] = 0x0056, /* non-US \| */
    [0x65] = (uint16_t)(HID_E0 | 0x5D), /* Application */
};

/* 修饰键位 -> set-1（bit0=LCTRL ... bit7=RGUI） */
static const uint16_t g_mod_set1[8] = {
    0x001D, 0x002A, 0x0038, (uint16_t)(HID_E0 | 0x5B),
    (uint16_t)(HID_E0 | 0x1D), 0x0036, (uint16_t)(HID_E0 | 0x38),
    (uint16_t)(HID_E0 | 0x5C),
};

static uint8_t g_report[USB_MAX_DEVICES][8];
static uint8_t g_prev[USB_MAX_DEVICES][8];

static void hid_kbd_make(uint16_t m)
{
    if (!m) {
        return;
    }
    if (m & HID_E0) {
        kbd_feed_byte(0xE0);
    }
    kbd_feed_byte((uint8_t)(m & 0xFF));
}

static void hid_kbd_break(uint16_t m)
{
    if (!m) {
        return;
    }
    if (m & HID_E0) {
        kbd_feed_byte(0xE0);
    }
    kbd_feed_byte((uint8_t)(0x80 | (m & 0xFF)));
}

static void hid_kbd_process(int idx, const uint8_t *rep, int len)
{
    if (len < 8) {
        return;
    }
    const uint8_t *prev = g_prev[idx];
    uint8_t mod = rep[0];
    uint8_t pmod = prev[0];

    /* 修饰键 */
    for (int i = 0; i < 8; i++) {
        int cb = (mod >> i) & 1;
        int pb = (pmod >> i) & 1;
        if (cb && !pb) {
            hid_kbd_make(g_mod_set1[i]);
        } else if (!cb && pb) {
            hid_kbd_break(g_mod_set1[i]);
        }
    }
    /* 普通键：新按下 */
    for (int i = 0; i < 6; i++) {
        uint8_t k = rep[2 + i];
        if (k == 0) {
            continue;
        }
        int found = 0;
        for (int j = 0; j < 6; j++) {
            if (prev[2 + j] == k) {
                found = 1;
                break;
            }
        }
        if (!found) {
            hid_kbd_make(g_hid_set1[k]);
        }
    }
    /* 普通键：已释放 */
    for (int i = 0; i < 6; i++) {
        uint8_t k = prev[2 + i];
        if (k == 0) {
            continue;
        }
        int found = 0;
        for (int j = 0; j < 6; j++) {
            if (rep[2 + j] == k) {
                found = 1;
                break;
            }
        }
        if (!found) {
            hid_kbd_break(g_hid_set1[k]);
        }
    }
    memcpy(g_prev[idx], rep, 8);
}

/* USB(HID) 鼠标垂直方向相对 PS/2 是否取反。
 * 用户要求「让 usb 鼠标和 ps2 鼠标的移动方向相反（上下相反）」→ 置 1：
 * 对 HID 报告的 dy 取反后再合成 PS/2 包，从而 USB 鼠标上下方向与 PS/2 相反。
 * 若日后要让两者一致，改回 0 即可。 */
#define USB_MOUSE_INVERT_Y 1

static void hid_mouse_process(int idx, const uint8_t *rep, int len)
{
    (void)idx;
    if (len < 3) {
        return;
    }
    uint8_t buttons = (uint8_t)(rep[0] & 0x07);
    int8_t dx = (int8_t)rep[1];
    int8_t dy = (int8_t)rep[2];

#if USB_MOUSE_INVERT_Y
    dy = (int8_t)(-dy);            /* 上下取反：与 PS/2 鼠标方向相反 */
#endif

    /* 合成 PS/2 包：bit3 恒为 1，低 3 位为按键 */
    uint8_t b0 = (uint8_t)(0x08 | buttons);
    uint8_t b1 = (uint8_t)dx;
    uint8_t b2 = (uint8_t)dy;
    mouse_feed_byte(b0);
    mouse_feed_byte(b1);
    mouse_feed_byte(b2);
    if (mouse_packet_len() >= 4) {
        uint8_t wheel = (len >= 4) ? rep[3] : 0;
        mouse_feed_byte(wheel);
    }
}

bool usb_hid_start(int dev_idx, uint8_t interface_num)
{
    usb_device_t *d = usb_core_get(dev_idx);
    if (!d) {
        return false;
    }
    uint8_t bm = (uint8_t)(USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE);
    /* boot 协议（wValue=0）：标准 8B 键盘 / 3B 鼠标报告 */
    uhci_control(d->address, d->low_speed, d->max_packet0, bm,
                 USB_HID_SET_PROTOCOL, 0, interface_num, NULL, 0);
    /* 空闲率 0：仅在状态变化上报 */
    uhci_control(d->address, d->low_speed, d->max_packet0, bm,
                 USB_HID_SET_IDLE, 0, interface_num, NULL, 0);

    int slot = uhci_int_add(d->address, d->low_speed, d->int_ep,
                            d->int_mps, 10, g_report[dev_idx], 8);
    if (slot < 0) {
        return false;
    }
    d->int_slot = (uint8_t)slot;
    memset(g_prev[dev_idx], 0, 8);
    memset(g_report[dev_idx], 0, 8);
    kprintf("[usb-hid] %s ready (addr=%u ep=%02x mps=%u)\n",
            d->kind == USB_KIND_HID_MOUSE ? "mouse" : "keyboard",
            (unsigned)d->address, d->int_ep, d->int_mps);
    return true;
}

int usb_hid_poll(int dev_idx)
{
    usb_device_t *d = usb_core_get(dev_idx);
    if (!d || d->int_slot == 0xFF) {
        return -1;
    }
    int n = uhci_int_poll(d->int_slot);
    if (n < 0) {
        return -1;                  /* 端点致命错误：交由 usb_core 释放设备 */
    }
    if (n == 0) {
        return 0;
    }
    const uint8_t *rep = g_report[dev_idx];
    if (d->kind == USB_KIND_HID_MOUSE) {
        hid_mouse_process(dev_idx, rep, n);
    } else {
        hid_kbd_process(dev_idx, rep, n);
    }
    return n;
}
