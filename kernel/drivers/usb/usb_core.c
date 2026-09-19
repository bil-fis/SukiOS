/*
 * kernel/drivers/usb/usb_core.c
 * -----------------------------------------------------------------------------
 * USB 核心：设备枚举（端点 0 控制管道）、设备表、类分派、即插即用服务任务。
 *
 * 枚举流程（OSDev「USB」标准做法）：
 *   读 8B 设备描述符(addr0) -> SET_ADDRESS -> 读全设备描述符 ->
 *   读配置描述符(9B 头 + 全量) -> 解析接口/端点 -> SET_CONFIGURATION ->
 *   按接口类分派（HID / Hub）。
 *
 * 即插即用：UHCI 根端口变化由 uhci_poll() 回调 usb_core_root_connect()；
 * Hub 下行端口变化由 usb_hub_poll() 回调 usb_core_hub_connect()。
 * 所有枚举在「USB 主机服务」内核任务上下文执行（非中断），可安全使用延时。
 */
#include <kernel/types.h>
#include <kernel/usb.h>
#include <kernel/uhci.h>
#include <kernel/usb_hid.h>
#include <kernel/usb_hub.h>
#include <kernel/io.h>
#include <kernel/console.h>
#include <kernel/string.h>
#include <kernel/task.h>

#define USB_SLOT_INVALID 0xFF

static usb_device_t g_dev[USB_MAX_DEVICES];
static uint8_t      g_addr_used[128];
static uint8_t      g_next_addr = 1;
static bool         g_hc_ready = false;

/* ~1us 微延迟 */
static void usb_us(uint32_t us)
{
    for (uint32_t i = 0; i < us; i++) {
        (void)inb(0x80);
    }
}
static void usb_ms(uint32_t ms)
{
    usb_us(ms * 1000);
}

static int usb_control(uint8_t addr, uint8_t ls, uint8_t mps,
                       uint8_t bmRequestType, uint8_t bRequest,
                       uint16_t wValue, uint16_t wIndex,
                       void *data, uint16_t len)
{
    return uhci_control(addr, ls, mps, bmRequestType, bRequest, wValue,
                        wIndex, data, len);
}

static uint8_t usb_alloc_addr(void)
{
    for (int i = 0; i < 127; i++) {
        uint8_t a = g_next_addr;
        g_next_addr++;
        if (g_next_addr > 126) {
            g_next_addr = 1;
        }
        if (!g_addr_used[a]) {
            g_addr_used[a] = 1;
            return a;
        }
    }
    return 0;
}

static void usb_free_addr(uint8_t a)
{
    if (a > 0 && a < 128) {
        g_addr_used[a] = 0;
    }
}

usb_device_t *usb_core_get(int idx)
{
    if (idx < 0 || idx >= USB_MAX_DEVICES) {
        return NULL;
    }
    return &g_dev[idx];
}

static int usb_find_free_slot(void)
{
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (g_dev[i].state == USB_STATE_FREE) {
            return i;
        }
    }
    return -1;
}

/* 解析配置描述符：取第一个接口的类/子类/协议与第一个中断 IN 端点。 */
static void usb_parse_config(const uint8_t *cfg, int len,
                             int *iclass, int *isub, int *iproto, int *inum,
                             uint8_t *ep, uint16_t *epmps, uint8_t *epint,
                             int *have_ep)
{
    int off = 0;
    *iclass = -1;
    *isub = 0;
    *iproto = 0;
    *inum = 0;
    *have_ep = 0;
    *ep = 0;
    *epmps = 8;
    *epint = 10;
    while (off + 2 <= len) {
        uint8_t blen = cfg[off];
        uint8_t btype = cfg[off + 1];
        if (blen < 2 || off + blen > len) {
            break;
        }
        if (btype == USB_DT_INTERFACE && blen >= 9) {
            const usb_interface_desc_t *id =
                (const usb_interface_desc_t *)(cfg + off);
            if (*iclass < 0) {
                *iclass = id->bInterfaceClass;
                *isub = id->bInterfaceSubClass;
                *iproto = id->bInterfaceProtocol;
                *inum = id->bInterfaceNumber;
            }
        } else if (btype == USB_DT_ENDPOINT && blen >= 7) {
            const usb_endpoint_desc_t *ed =
                (const usb_endpoint_desc_t *)(cfg + off);
            if ((ed->bmAttributes & 0x03) == 0x03 &&
                (ed->bEndpointAddress & 0x80) && !*have_ep) {
                *ep = ed->bEndpointAddress;
                *epmps = (uint16_t)(ed->wMaxPacketSize & 0x7FF);
                if (*epmps == 0) {
                    *epmps = 8;
                }
                *epint = ed->bInterval;
                *have_ep = 1;
            }
        }
        off += blen;
    }
}

static void usb_core_free(int idx);

/* 枚举一个新设备（addr0 -> 分配地址 -> 配置 -> 分派）。成功返回下标。 */
static int usb_core_enum(bool low_speed, int parent_hub, int parent_port)
{
    int idx = usb_find_free_slot();
    if (idx < 0) {
        kprintf("[usb] device table full, ignore new device\n");
        return -1;
    }
    usb_device_t *d = &g_dev[idx];
    memset(d, 0, sizeof(*d));
    d->state = USB_STATE_FREE;
    d->parent_hub = parent_hub;
    d->parent_port = (uint8_t)parent_port;
    d->low_speed = low_speed ? 1 : 0;
    d->int_slot = USB_SLOT_INVALID;
    d->kind = USB_KIND_NONE;

    uint8_t buf[512];
    int r = -1;

    /* 1) 地址 0 读 8 字节设备描述符（重试，热插拔时设备首次响应可能偏慢） */
    for (int tries = 0; tries < 12; tries++) {
        r = usb_control(0, d->low_speed, 8,
                        USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                        USB_REQ_GET_DESCRIPTOR, 0x0100, 0, buf, 8);
        if (r >= 8) {
            break;
        }
        usb_ms(20);
    }
    if (r < 8) {
        kprintf("[usb] enum: no device descriptor at addr0 (r=%d)\n", r);
        return -1;
    }
    uint8_t mps0 = buf[7];
    if (mps0 == 0) {
        mps0 = 8;
    }

    /* 2) SET_ADDRESS */
    uint8_t addr = usb_alloc_addr();
    if (!addr) {
        kprintf("[usb] enum: no free USB address\n");
        return -1;
    }
    r = usb_control(0, d->low_speed, mps0,
                    USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                    USB_REQ_SET_ADDRESS, addr, 0, NULL, 0);
    if (r < 0) {
        kprintf("[usb] enum: SET_ADDRESS failed\n");
        usb_free_addr(addr);
        return -1;
    }
    usb_ms(5);
    d->address = addr;
    d->max_packet0 = mps0;

    /* 3) 全设备描述符 */
    r = usb_control(addr, d->low_speed, mps0,
                    USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                    USB_REQ_GET_DESCRIPTOR, 0x0100, 0, buf, 18);
    if (r < 18) {
        kprintf("[usb] enum: full device descriptor failed (r=%d)\n", r);
        usb_free_addr(addr);
        d->address = 0;
        return -1;
    }
    const usb_device_desc_t *dd = (const usb_device_desc_t *)buf;
    d->dev_class = dd->bDeviceClass;
    uint16_t vid = dd->idVendor;
    uint16_t pid = dd->idProduct;

    /* 4) 配置描述符（先 9 字节头，再全量） */
    uint8_t chdr[9];
    r = usb_control(addr, d->low_speed, mps0,
                    USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                    USB_REQ_GET_DESCRIPTOR, 0x0200, 0, chdr, 9);
    if (r < 9) {
        kprintf("[usb] enum: config header failed (r=%d)\n", r);
        usb_free_addr(addr);
        d->address = 0;
        return -1;
    }
    const usb_config_desc_t *cd = (const usb_config_desc_t *)chdr;
    uint16_t total = cd->wTotalLength;
    if (total < 9) {
        total = 9;
    }
    if (total > 480) {
        total = 480;
    }
    d->config_value = cd->bConfigurationValue;
    int cfg_len = usb_control(addr, d->low_speed, mps0,
                              USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                              USB_REQ_GET_DESCRIPTOR, 0x0200, 0, buf, total);
    if (cfg_len < 9) {
        kprintf("[usb] enum: config descriptor failed (r=%d)\n", cfg_len);
        usb_free_addr(addr);
        d->address = 0;
        return -1;
    }

    int iclass, isub, iproto, inum, have_ep;
    uint8_t ep; uint16_t epmps; uint8_t epint;
    usb_parse_config(buf, cfg_len, &iclass, &isub, &iproto, &inum,
                     &ep, &epmps, &epint, &have_ep);

    /* 5) SET_CONFIGURATION */
    r = usb_control(addr, d->low_speed, mps0,
                    USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                    USB_REQ_SET_CONFIGURATION, d->config_value, 0, NULL, 0);
    if (r < 0) {
        kprintf("[usb] enum: SET_CONFIGURATION failed\n");
        usb_free_addr(addr);
        d->address = 0;
        return -1;
    }

    kprintf("[usb] device addr=%u %s vid=%04x pid=%04x class=%d"
            " iface=%d/%d/%d ep=%02x mps=%u\n",
            (unsigned)addr, d->low_speed ? "LS" : "FS",
            vid, pid, iclass, iclass, isub, iproto, ep, epmps);

    /* 6) 类分派 */
    if (iclass == USB_CLASS_HID && have_ep) {
        d->kind = (iproto == 2) ? USB_KIND_HID_MOUSE : USB_KIND_HID_KBD;
        d->hid_protocol = (uint8_t)(iproto ? iproto : 1);
        d->int_ep = ep;
        d->int_mps = epmps;
        if (usb_hid_start(idx, (uint8_t)inum)) {
            d->state = USB_STATE_ENUMERATED;
            return idx;
        }
        kprintf("[usb] HID start failed\n");
    } else if (iclass == USB_CLASS_HUB) {
        d->kind = USB_KIND_HUB;
        if (usb_hub_start(idx)) {
            d->state = USB_STATE_ENUMERATED;
            return idx;
        }
        kprintf("[usb] hub start failed\n");
    } else {
        /* 未支持的类：保持已配置但无驱动（不影响系统） */
        d->state = USB_STATE_ENUMERATED;
        return idx;
    }

    /* 启动失败：回滚 */
    usb_free_addr(addr);
    d->address = 0;
    return -1;
}

/* 释放设备（含其所有下游子设备）。 */
static void usb_core_free(int idx)
{
    if (idx < 0 || idx >= USB_MAX_DEVICES) {
        return;
    }
    usb_device_t *d = &g_dev[idx];
    if (d->state == USB_STATE_FREE && d->kind == USB_KIND_NONE &&
        d->int_slot == 0) {
        return;
    }
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (i != idx && g_dev[i].state != USB_STATE_FREE &&
            g_dev[i].parent_hub == idx) {
            usb_core_free(i);
        }
    }
    if (d->kind == USB_KIND_HUB) {
        usb_hub_cleanup(idx);
    }
    if (d->int_slot != USB_SLOT_INVALID) {
        uhci_int_remove(d->int_slot);
    }
    if (d->address) {
        usb_free_addr(d->address);
    }
    memset(d, 0, sizeof(*d));
    d->state = USB_STATE_FREE;
    d->int_slot = USB_SLOT_INVALID;
}

/* --------------------------- 即插即用回调 --------------------------- */

int usb_core_root_connect(int port, bool low_speed)
{
    if (!g_hc_ready) {
        return -1;
    }
    /* 同一根端口若已有设备，先移除 */
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (g_dev[i].state != USB_STATE_FREE && g_dev[i].parent_hub < 0 &&
            g_dev[i].parent_port == port) {
            usb_core_free(i);
        }
    }
    int idx = usb_core_enum(low_speed, -1, port);
    if (idx >= 0) {
        kprintf("[usb] root port %d: device attached (%s)\n",
                port, low_speed ? "low-speed" : "full-speed");
    }
    return idx;
}

void usb_core_root_disconnect(int port)
{
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (g_dev[i].state != USB_STATE_FREE && g_dev[i].parent_hub < 0 &&
            g_dev[i].parent_port == port) {
            kprintf("[usb] root port %d: device detached\n", port);
            usb_core_free(i);
        }
    }
}

int usb_core_hub_connect(int hub_idx, int port, bool low_speed)
{
    if (!g_hc_ready || hub_idx < 0 || hub_idx >= USB_MAX_DEVICES) {
        return -1;
    }
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (g_dev[i].state != USB_STATE_FREE && g_dev[i].parent_hub == hub_idx &&
            g_dev[i].parent_port == port) {
            usb_core_free(i);
        }
    }
    int idx = usb_core_enum(low_speed, hub_idx, port);
    if (idx >= 0) {
        kprintf("[usb] hub %d port %d: device attached (%s)\n",
                hub_idx, port, low_speed ? "low-speed" : "full-speed");
    }
    return idx;
}

void usb_core_hub_disconnect(int hub_idx, int port)
{
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (g_dev[i].state != USB_STATE_FREE && g_dev[i].parent_hub == hub_idx &&
            g_dev[i].parent_port == port) {
            kprintf("[usb] hub %d port %d: device detached\n", hub_idx, port);
            usb_core_free(i);
        }
    }
}

/* ------------------------------ 轮询 ------------------------------ */

void usb_poll(void)
{
    if (!g_hc_ready) {
        return;
    }
    uhci_poll();                       /* 根端口变化 -> 枚举 */
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        usb_device_t *d = &g_dev[i];
        if (d->state != USB_STATE_ENUMERATED) {
            continue;
        }
        if (d->kind == USB_KIND_HID_KBD || d->kind == USB_KIND_HID_MOUSE) {
            usb_hid_poll(i);
        } else if (d->kind == USB_KIND_HUB) {
            usb_hub_poll(i);
        }
    }
}

/* USB 主机服务内核任务：周期性轮询（在进程上下文，可安全延时）。 */
static void usb_service_task(void *arg)
{
    (void)arg;
    for (;;) {
        usb_poll();
        usb_ms(8);
    }
}

bool usb_init(void)
{
    if (g_hc_ready) {
        return true;
    }
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        memset(&g_dev[i], 0, sizeof(g_dev[i]));
        g_dev[i].state = USB_STATE_FREE;
        g_dev[i].int_slot = USB_SLOT_INVALID;
    }
    memset(g_addr_used, 0, sizeof(g_addr_used));

    if (!uhci_init()) {
        kprintf("[usb] no host controller, USB stack disabled\n");
        return false;
    }
    g_hc_ready = true;

    task_t *t = task_create_kernel(usb_service_task, NULL, "SukiUsbHost");
    if (!t) {
        kprintf("[usb] failed to start host service task\n");
        return false;
    }
    kprintf("[usb] host service started (UHCI, polling)\n");
    return true;
}
