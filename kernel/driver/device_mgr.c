/*
 * kernel/driver/device_mgr.c
 * -----------------------------------------------------------------------------
 * 设备管理器（Device Manager）
 *
 * 职责：
 *   - 维护已发现设备（device_t）的全局链表；
 *   - 提供 device_register()，触发「双向匹配」：让本设备尝试匹配所有已注册驱动；
 *   - device_manager_scan_pci()：枚举 PCI 总线，把每个有效功能注册为 device_t；
 *   - device_manager_add_demo()：注册虚拟演示设备（用于验证「一套驱动驱动多个设备」）；
 *   - 与 driver_mgr 协作完成绑定：一次 probe() 对应一个设备实例，驱动内用 kmalloc
 *     分配独立 drvdata，从而同一份 driver_t 可服务多个 device_t。
 */
#include <kernel/types.h>
#include <kernel/console.h>
#include <kernel/spinlock.h>
#include <mm/kmalloc.h>
#include <kernel/pci.h>
#include <kernel/string.h>
#include <sukios/kdr.h>

static device_t *g_devices = NULL;
static spinlock_t g_dlock;
static uint32_t g_next_dev_id = 1;

/* 把一个驱动尝试绑定到所有「未绑定」的设备；匹配成功则 probe 一次 */
void device_try_bind_driver(driver_t *drv)
{
    if (!drv)
        return;
    spin_lock(&g_dlock);
    for (device_t *d = g_devices; d; d = d->next) {
        if (d->driver)
            continue;
        if (drv->match && drv->match(d)) {
            int rc = drv->probe ? drv->probe(d) : -1;
            if (rc == 0) {
                d->driver = drv;
                drv->nbound++;
                kprintf("[device] '%s' (id #%u) bound to driver '%s'  [nbound=%d]\n",
                        d->name ? d->name : "?", d->dev_id,
                        drv->name ? drv->name : "?", drv->nbound);
            } else {
                kprintf("[device] driver '%s' probe '%s' failed rc=%d\n",
                        drv->name ? drv->name : "?",
                        d->name ? d->name : "?", rc);
            }
        }
    }
    spin_unlock(&g_dlock);
}

/* 让一个设备尝试匹配所有驱动（设备注册时调用） */
void device_try_bind_all_drivers(device_t *dev)
{
    if (!dev)
        return;
    spin_lock(&g_dlock);
    for (driver_t *drv = driver_list_head(); drv; drv = drv->next) {
        if (dev->driver)
            break;
        if (drv->match && drv->match(dev)) {
            int rc = drv->probe ? drv->probe(dev) : -1;
            if (rc == 0) {
                dev->driver = drv;
                drv->nbound++;
                kprintf("[device] '%s' (id #%u) bound to driver '%s'  [nbound=%d]\n",
                        dev->name ? dev->name : "?", dev->dev_id,
                        drv->name ? drv->name : "?", drv->nbound);
            } else {
                kprintf("[device] driver '%s' probe '%s' failed rc=%d\n",
                        drv->name ? drv->name : "?",
                        dev->name ? dev->name : "?", rc);
            }
        }
    }
    spin_unlock(&g_dlock);
}

int device_register(device_t *dev)
{
    if (!dev)
        return -1;
    dev->dev_id = g_next_dev_id++;
    dev->driver = NULL;
    dev->drvdata = NULL;
    spin_lock(&g_dlock);
    dev->next = g_devices;
    g_devices = dev;
    spin_unlock(&g_dlock);

    kprintf("[device] registered #%u '%s' bus=%d cls=%02x/%02x\n",
            dev->dev_id, dev->name ? dev->name : "?",
            (int)dev->bus, dev->class_code, dev->subclass);

    device_try_bind_all_drivers(dev);
    return 0;
}

void device_unregister(device_t *dev)
{
    if (!dev)
        return;
    spin_lock(&g_dlock);
    device_t **pp = &g_devices;
    while (*pp) {
        if (*pp == dev) {
            *pp = dev->next;
            break;
        }
        pp = &(*pp)->next;
    }
    spin_unlock(&g_dlock);
    kprintf("[device] unregistered #%u '%s'\n", dev->dev_id, dev->name ? dev->name : "?");
}

void device_manager_init(void)
{
    g_devices = NULL;
    spinlock_init(&g_dlock, "device-mgr");
    kprintf("[device] manager initialized\n");
}

/* ---------------- 虚拟演示设备 ---------------- */
void device_manager_add_demo(uint32_t id, const char *name)
{
    device_t *d = kmalloc(sizeof(device_t));
    if (!d) {
        kprintf("[device] demo alloc failed\n");
        return;
    }
    memset(d, 0, sizeof(*d));
    d->bus = BUS_VIRTUAL;
    d->class_code = 0x80;   /* 自定义演示类 */
    d->subclass   = 0x01;
    d->name = name;
    d->dev_id = id;
    device_register(d);
}

/* ---------------- PCI 枚举 ---------------- */
static void hex4(char *b, uint16_t v)
{
    static const char *d = "0123456789abcdef";
    b[0] = d[(v >> 12) & 0xF];
    b[1] = d[(v >> 8) & 0xF];
    b[2] = d[(v >> 4) & 0xF];
    b[3] = d[v & 0xF];
    b[4] = 0;
}

static void pci_register_one(uint8_t bus, uint8_t dev, uint8_t func)
{
    uint32_t id = pci_cfg_read32(bus, dev, func, 0x00);
    uint16_t vid = id & 0xFFFF;
    if (vid == 0xFFFF)
        return;
    uint16_t did = (id >> 16) & 0xFFFF;
    uint32_t rcls = pci_cfg_read32(bus, dev, func, 0x08);
    uint8_t cls  = (rcls >> 24) & 0xFF;
    uint8_t sub  = (rcls >> 16) & 0xFF;
    uint32_t bar0 = pci_cfg_read32(bus, dev, func, 0x10);
    uint64_t mmio = 0;
    if ((bar0 & 1) == 0 && bar0) {           /* 仅取首个 MMIO 32/64 位 BAR */
        mmio = (uint64_t)(bar0 & 0xFFFFFFF0);
        if (bar0 & 0x4) {
            uint32_t bar1 = pci_cfg_read32(bus, dev, func, 0x14);
            mmio |= ((uint64_t)bar1) << 32;
        }
    }

    device_t *d = kmalloc(sizeof(device_t));
    if (!d)
        return;
    memset(d, 0, sizeof(*d));
    d->bus = BUS_PCI;
    d->vendor = vid; d->device = did;
    d->class_code = cls; d->subclass = sub;
    d->bus_no = bus; d->dev_no = dev; d->func = func;
    d->mmio_base = mmio;

    char *nm = kmalloc(20);
    if (nm) {
        hex4(nm, vid);
        nm[4] = ':';
        hex4(nm + 5, did);
    }
    d->name = nm ? nm : "pci";

    /* PCIe 标注：能力链表含 cap 0x10 即为 PCIe 设备/端口（osdev "PCI Express"）。
     * 传统 PCI 平台（-machine pc / i440FX）通常全部为 PCI，无此能力；
     * 具备 MCFG 的平台（q35/PCIe）会命中并打印设备/端口类型。 */
    {
        pci_dev_t pd = { bus, dev, func, vid, did };
        if (pci_find_cap(&pd, PCI_CAP_PCIE, NULL)) {
            kprintf("[device]   ^ PCIe device %02x:%02x.%u type=%u\n",
                    (unsigned)bus, (unsigned)dev, (unsigned)func,
                    (unsigned)pci_pcie_type(&pd));
        }
    }

    device_register(d);
}

void device_manager_scan_pci(void)
{
    kprintf("[device] scanning PCI buses 0..7 ...\n");
    for (uint8_t bus = 0; bus < 8; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint16_t vid = pci_cfg_read32(bus, dev, 0, 0x00) & 0xFFFF;
            if (vid == 0xFFFF)
                continue;
            pci_register_one(bus, dev, 0);
            uint8_t htype = (pci_cfg_read32(bus, dev, 0, 0x0C) >> 16) & 0x80;
            if (htype) {
                for (uint8_t f = 1; f < 8; f++) {
                    uint16_t fv = pci_cfg_read32(bus, dev, f, 0x00) & 0xFFFF;
                    if (fv != 0xFFFF)
                        pci_register_one(bus, dev, f);
                }
            }
        }
    }
    kprintf("[device] PCI scan complete\n");
}
