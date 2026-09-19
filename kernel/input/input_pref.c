/*
 * kernel/input/input_pref.c
 * -----------------------------------------------------------------------------
 * 输入源优先级仲裁（USB 优先 / PS/2 回退）。实现见 include/kernel/input_pref.h。
 */
#include <kernel/input_pref.h>

static volatile bool g_usb_kbd = false;
static volatile bool g_usb_mouse = false;

void input_pref_set_usb_kbd(bool active)
{
    g_usb_kbd = active;
}

void input_pref_set_usb_mouse(bool active)
{
    g_usb_mouse = active;
}

bool input_pref_usb_kbd_active(void)
{
    return g_usb_kbd;
}

bool input_pref_usb_mouse_active(void)
{
    return g_usb_mouse;
}
