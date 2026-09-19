/*
 * kernel/drivers/usb/usb_hub.c
 * -----------------------------------------------------------------------------
 * USB Hub 类驱动（OSDev「USB Hubs」）。
 *
 *   - GET_DESCRIPTOR(HUB,0x29) 得下行端口数 bNbrPorts；
 *   - 为每个端口 SET_FEATURE(PORT_POWER) 供电；
 *   - 周期轮询每端口 GET_STATUS（Class/Other），据 wPortChange 处理
 *     连接/断开：复位端口 -> usb_core_hub_connect 枚举下游设备。
 *
 * 端口状态位：bit0 CONNECTION, bit1 ENABLE, bit4 RESET, bit8 POWER, bit9 LOW_SPEED。
 * 端口变化位：bit0 C_PORT_CONNECTION, bit1 C_PORT_ENABLE, bit4 C_PORT_RESET。
 */
#include <kernel/types.h>
#include <kernel/usb_hub.h>
#include <kernel/usb.h>
#include <kernel/uhci.h>
#include <kernel/io.h>
#include <kernel/console.h>
#include <kernel/string.h>

#define HUB_PORT_CONNECTION   (1u << 0)
#define HUB_PORT_ENABLE       (1u << 1)
#define HUB_PORT_RESET        (1u << 4)
#define HUB_PORT_POWER        (1u << 8)
#define HUB_PORT_LOW_SPEED    (1u << 9)

#define HUB_CHG_CONNECTION    (1u << 0)
#define HUB_CHG_ENABLE        (1u << 1)
#define HUB_CHG_RESET         (1u << 4)

static void hub_us(uint32_t us)
{
    for (uint32_t i = 0; i < us; i++) {
        (void)inb(0x80);
    }
}

static int hub_ctrl(usb_device_t *d, uint8_t bm, uint8_t req,
                    uint16_t value, uint16_t index, void *data, uint16_t len)
{
    return uhci_control(d->address, d->low_speed, d->max_packet0,
                        bm, req, value, index, data, len);
}

static uint16_t hub_port_status(usb_device_t *d, int port, uint16_t *change)
{
    uint8_t st[4] = {0, 0, 0, 0};
    int r = hub_ctrl(d, (uint8_t)(USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER),
                     USB_HUB_GET_STATUS, 0, (uint16_t)port, st, 4);
    if (r < 4) {
        if (change) {
            *change = 0;
        }
        return 0;
    }
    if (change) {
        *change = (uint16_t)(st[2] | (st[3] << 8));
    }
    return (uint16_t)(st[0] | (st[1] << 8));
}

static void hub_clear_feature(usb_device_t *d, int port, uint16_t feature)
{
    hub_ctrl(d, (uint8_t)(USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_OTHER),
             USB_HUB_CLEAR_FEATURE, feature, (uint16_t)port, NULL, 0);
}

static void hub_set_feature(usb_device_t *d, int port, uint16_t feature)
{
    hub_ctrl(d, (uint8_t)(USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_OTHER),
             USB_HUB_SET_FEATURE, feature, (uint16_t)port, NULL, 0);
}

bool usb_hub_start(int dev_idx)
{
    usb_device_t *d = usb_core_get(dev_idx);
    if (!d) {
        return false;
    }
    uint8_t hd[16];
    int r = hub_ctrl(d, (uint8_t)(USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE),
                     USB_HUB_GET_DESCRIPTOR, (uint16_t)(USB_DT_HUB << 8), 0,
                     hd, 9);
    if (r < 3) {
        kprintf("[usb-hub] GET hub descriptor failed (r=%d)\n", r);
        return false;
    }
    uint8_t nports = hd[2];
    if (nports == 0 || nports > 15) {
        nports = 4;                         /* 与 xHCI 一致的保守默认 */
    }
    d->hub_ports = nports;

    /* 为所有下行端口供电 */
    for (int p = 1; p <= nports; p++) {
        hub_set_feature(d, p, USB_HUB_FEAT_PORT_POWER);
    }
    hub_us(100000);                         /* 等待供电稳定（100ms） */

    kprintf("[usb-hub] ready: addr=%u ports=%u\n",
            (unsigned)d->address, (unsigned)nports);
    return true;
}

/* 节流：主机服务任务约每 8ms 调一次，此处每 ~6 次（约 50ms）实际轮询一次。 */
static uint8_t g_hub_div[USB_MAX_DEVICES];

void usb_hub_poll(int dev_idx)
{
    usb_device_t *d = usb_core_get(dev_idx);
    if (!d || d->hub_ports == 0) {
        return;
    }
    g_hub_div[dev_idx]++;
    if (g_hub_div[dev_idx] < 6) {
        return;
    }
    g_hub_div[dev_idx] = 0;

    for (int p = 1; p <= (int)d->hub_ports; p++) {
        uint16_t change = 0;
        uint16_t status = hub_port_status(d, p, &change);
        if (change == 0) {
            continue;
        }
        if (change & HUB_CHG_CONNECTION) {
            hub_clear_feature(d, p, USB_HUB_FEAT_C_PORT_CONNECTION);
            status = hub_port_status(d, p, NULL);
            if (status & HUB_PORT_CONNECTION) {
                /* 新设备：复位端口后枚举 */
                hub_set_feature(d, p, USB_HUB_FEAT_PORT_RESET);
                hub_us(50000);              /* 复位保持 50ms */
                for (int t = 0; t < 20; t++) {
                    uint16_t st2 = hub_port_status(d, p, NULL);
                    if (!(st2 & HUB_PORT_RESET)) {
                        break;
                    }
                    hub_us(10000);
                }
                hub_clear_feature(d, p, USB_HUB_FEAT_C_PORT_RESET);
                status = hub_port_status(d, p, NULL);
                bool ls = (status & HUB_PORT_LOW_SPEED) ? true : false;
                usb_core_hub_connect(dev_idx, p, ls);
            } else {
                usb_core_hub_disconnect(dev_idx, p);
            }
        }
        if (change & HUB_CHG_RESET) {
            hub_clear_feature(d, p, USB_HUB_FEAT_C_PORT_RESET);
        }
        if (change & HUB_CHG_ENABLE) {
            hub_clear_feature(d, p, USB_HUB_FEAT_C_PORT_ENABLE);
        }
    }
}

void usb_hub_cleanup(int dev_idx)
{
    if (dev_idx >= 0 && dev_idx < USB_MAX_DEVICES) {
        g_hub_div[dev_idx] = 0;
    }
}
