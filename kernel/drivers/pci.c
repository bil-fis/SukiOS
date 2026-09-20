/*
 * kernel/drivers/pci.c
 * -----------------------------------------------------------------------------
 * PCI 配置空间访问（传统 I/O 端口机制）。
 *
 * CONFIG_ADDRESS (0xCF8) 格式：
 *   bit31    = Enable
 *   bit23:16 = Bus
 *   bit15:11 = Device
 *   bit10:8  = Function
 *   bit7:2   = 寄存器双字偏移（低 2 位必须为 0）
 * 随后从 CONFIG_DATA (0xCFC) 读/写 32 位；8/16 位访问通过双字内偏移拼出。
 *
 * 调用关系：hda_init() -> pci_find_class() -> pci_bar_addr()/pci_enable_device()。
 */
#include <kernel/pci.h>
#include <kernel/io.h>
#include <kernel/console.h>
#include <kernel/acpi.h>   /* PHYS_TO_VIRT + acpi_get_mcfg_window（P0-1 ECAM） */

#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

/* P0-1：ECAM 状态。g_ecam_base==0 表示平台未提供 MCFG（传统 PCI i440FX），
 * 此时所有配置访问走 PIO 0xCF8；否则走 MMIO ECAM（覆盖 PCIe 扩展配置空间）。 */
static uint64_t g_ecam_base = 0;
static uint8_t  g_ecam_bus_start = 0;
static uint8_t  g_ecam_bus_end = 0xFF;

void pci_cfg_init(void)
{
    uint8_t bs = 0, be = 0xFF;
    if (acpi_get_mcfg_window(0, 0, &g_ecam_base, &bs, &be)) {
        g_ecam_bus_start = bs;
        g_ecam_bus_end   = be;
        kprintf("[pci] ECAM enabled: base=%p bus=%u..%u\n",
                (void *)(uintptr_t)g_ecam_base, (unsigned)bs, (unsigned)be);
    } else {
        g_ecam_base = 0;
        kprintf("[pci] no MCFG, using PIO 0xCF8 config access\n");
    }
}

/* ECAM 线性地址：base + (bus<<20) + (dev<<15) + (func<<12) + off。
 * 仅当总线落在已声明窗口内时使用。 */
static inline void *ecam_ptr(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    uint64_t va = (uint64_t)(uintptr_t)PHYS_TO_VIRT(g_ecam_base);
    va += ((uint64_t)(bus - g_ecam_bus_start) << 20)
        | ((uint64_t)(dev & 0x1F) << 15)
        | ((uint64_t)(func & 0x7) << 12)
        | (off & 0xFFF);
    return (void *)va;
}

static inline bool ecam_usable(uint8_t bus)
{
    return g_ecam_base != 0 && bus >= g_ecam_bus_start && bus <= g_ecam_bus_end;
}

static inline uint32_t cfg_addr(uint8_t bus, uint8_t dev, uint8_t func,
                                uint8_t off)
{
    return 0x80000000U
         | ((uint32_t)bus  << 16)
         | ((uint32_t)(dev & 0x1F) << 11)
         | ((uint32_t)(func & 0x7) << 8)
         | ((uint32_t)off & 0xFC);
}

uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    if (ecam_usable(bus)) {
        return *(const volatile uint32_t *)ecam_ptr(bus, dev, func, off);
    }
    outl(PCI_CONFIG_ADDRESS, cfg_addr(bus, dev, func, off));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_cfg_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    if (ecam_usable(bus)) {
        return *(const volatile uint16_t *)ecam_ptr(bus, dev, func, off);
    }
    uint32_t v = pci_cfg_read32(bus, dev, func, off);
    return (uint16_t)(v >> ((off & 2) * 8));
}

uint8_t pci_cfg_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    if (ecam_usable(bus)) {
        return *(const volatile uint8_t *)ecam_ptr(bus, dev, func, off);
    }
    uint32_t v = pci_cfg_read32(bus, dev, func, off);
    return (uint8_t)(v >> ((off & 3) * 8));
}

void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off,
                     uint32_t val)
{
    if (ecam_usable(bus)) {
        *(volatile uint32_t *)ecam_ptr(bus, dev, func, off) = val;
        return;
    }
    outl(PCI_CONFIG_ADDRESS, cfg_addr(bus, dev, func, off));
    outl(PCI_CONFIG_DATA, val);
}

void pci_cfg_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off,
                     uint16_t val)
{
    if (ecam_usable(bus)) {
        *(volatile uint16_t *)ecam_ptr(bus, dev, func, off) = val;
        return;
    }
    uint32_t old = pci_cfg_read32(bus, dev, func, off);
    uint32_t shift = (off & 2) * 8;
    uint32_t nv = (old & ~(0xFFFFU << shift)) | ((uint32_t)val << shift);
    pci_cfg_write32(bus, dev, func, off, nv);
}

/* 扫描 bus 0..255 × dev 0..31 × func 0..7（多功能设备按 header bit7 判断） */
bool pci_find_class(uint8_t class_code, uint8_t subclass, pci_dev_t *out)
{
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint16_t vid = pci_cfg_read16((uint8_t)bus, dev, 0,
                                          PCI_CFG_VENDOR_ID);
            if (vid == 0xFFFF) {
                continue;                       /* 无设备 */
            }
            uint8_t nfunc = 1;
            if (pci_cfg_read8((uint8_t)bus, dev, 0, PCI_CFG_HEADER_TYPE)
                    & 0x80) {
                nfunc = 8;                      /* 多功能设备 */
            }
            for (uint8_t fn = 0; fn < nfunc; fn++) {
                uint16_t v = pci_cfg_read16((uint8_t)bus, dev, fn,
                                            PCI_CFG_VENDOR_ID);
                if (v == 0xFFFF) {
                    continue;
                }
                uint8_t cc = pci_cfg_read8((uint8_t)bus, dev, fn,
                                           PCI_CFG_CLASS);
                uint8_t sc = pci_cfg_read8((uint8_t)bus, dev, fn,
                                           PCI_CFG_SUBCLASS);
                if (cc == class_code && sc == subclass) {
                    out->bus = (uint8_t)bus;
                    out->dev = dev;
                    out->func = fn;
                    out->vendor_id = v;
                    out->device_id = pci_cfg_read16((uint8_t)bus, dev, fn,
                                                    PCI_CFG_DEVICE_ID);
                    return true;
                }
            }
        }
    }
    return false;
}

uint64_t pci_bar_addr(const pci_dev_t *d, uint8_t bar_index)
{
    if (bar_index > 5) {
        return 0;
    }
    uint8_t off = (uint8_t)(PCI_CFG_BAR0 + bar_index * 4);
    uint32_t lo = pci_cfg_read32(d->bus, d->dev, d->func, off);
    if (lo & 0x1) {
        return 0;                               /* I/O BAR，不支持 */
    }
    uint64_t addr = lo & 0xFFFFFFF0U;
    if (((lo >> 1) & 0x3) == 0x2) {             /* 64 位 MMIO BAR */
        uint32_t hi = pci_cfg_read32(d->bus, d->dev, d->func,
                                     (uint8_t)(off + 4));
        addr |= ((uint64_t)hi << 32);
    }
    return addr;
}

void pci_enable_device(const pci_dev_t *d)
{
    uint16_t cmd = pci_cfg_read16(d->bus, d->dev, d->func, PCI_CFG_COMMAND);
    cmd |= PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER;
    pci_cfg_write16(d->bus, d->dev, d->func, PCI_CFG_COMMAND, cmd);
}

/* P0-1/R8：解析 PCI 设备 INTx# 的中断 GSI。
 * 优先走 _PRT（acpi_pci_route）；若 _PRT 缺失或不可解析，回落固件预编程的
 * INT_LINE(0x3C)。int_pin 为配置空间 0x3D 的值（1=A..4=D）；0 表示无中断。 */
int pci_route_interrupt(const pci_dev_t *dev, uint8_t int_pin)
{
    if (!dev || int_pin < 1 || int_pin > 4) {
        return -1;
    }
    int gsi = acpi_pci_route(dev->bus, dev->dev, (uint8_t)(int_pin - 1));
    if (gsi >= 0) {
        return gsi;
    }
    uint8_t line = pci_cfg_read8(dev->bus, dev->dev, dev->func, 0x3C);
    return (line != 0) ? (int)line : -1;
}

/* P0-2/R6：遍历 PCI 能力链表查找 cap_id。命中返回 true 且 *off_out 为能力
 * 寄存器在配置空间中的偏移；否则返回 false。 */
bool pci_find_cap(const pci_dev_t *dev, uint8_t cap_id, uint8_t *off_out)
{
    if (!dev) {
        return false;
    }
    uint16_t status = pci_cfg_read16(dev->bus, dev->dev, dev->func, 0x06);
    if (!(status & 0x10)) {            /* 状态寄存器 bit4：能力链表存在 */
        return false;
    }
    uint8_t off = pci_cfg_read8(dev->bus, dev->dev, dev->func, 0x34);
    for (uint8_t step = 0; off != 0 && step < 32; step++) {
        if (off < 0x40 || off > 0xFC) {        /* 防御越界 */
            break;
        }
        uint8_t id = pci_cfg_read8(dev->bus, dev->dev, dev->func, off);
        if (id == cap_id) {
            if (off_out) *off_out = off;
            return true;
        }
        off = pci_cfg_read8(dev->bus, dev->dev, dev->func, (uint8_t)(off + 1));
    }
    return false;
}

/* P0 驱动框架：PCIe 能力探测（见 include/kernel/pci.h 说明）。 */
bool pci_is_pcie(const pci_dev_t *dev)
{
    return pci_find_cap(dev, PCI_CAP_PCIE, NULL);
}

uint8_t pci_pcie_type(const pci_dev_t *dev)
{
    uint8_t off;
    if (!pci_find_cap(dev, PCI_CAP_PCIE, &off)) {
        return 0;
    }
    uint16_t cap = pci_cfg_read16(dev->bus, dev->dev, dev->func,
                                  (uint8_t)(off + 2));
    return (uint8_t)((cap >> 4) & 0xF);
}

bool pci_msi_capable(const pci_dev_t *dev, bool *has_msix)
{
    if (has_msix) *has_msix = false;
    if (!dev) {
        return false;
    }
    if (pci_find_cap(dev, PCI_CAP_MSIX, NULL)) {
        if (has_msix) *has_msix = true;
        return true;
    }
    return pci_find_cap(dev, PCI_CAP_MSI, NULL);
}

/* 构造 MSI Message Address：0xFEE00000 | (DestID<<12)，物理目标、无重定向提示。 */
static uint64_t msi_addr(uint8_t lapic_id)
{
    return 0xFEE00000ULL | ((uint64_t)(lapic_id & 0xFF) << 12);
}

/* P0-2/R6：在设备上启用消息信号中断（优先 MSI-X，否则 MSI）。
 * 把中断投递到 lapic_id 的 vector 号（固定、边沿触发）。成功返回 true，
 * 调用方应改用该 MSI 向量、不再配置 IOAPIC；不支持或失败返回 false（回落 IOAPIC）。 */
bool pci_enable_msi(const pci_dev_t *dev, uint8_t vector, uint8_t lapic_id)
{
    if (!dev) {
        return false;
    }
    uint8_t off;

    /* —— MSI-X（cap 0x11） —— */
    if (pci_find_cap(dev, PCI_CAP_MSIX, &off)) {
        uint32_t tbl = pci_cfg_read32(dev->bus, dev->dev, dev->func,
                                      (uint8_t)(off + 4));
        uint8_t bir = (uint8_t)(tbl & 0x7);
        uint32_t tbl_off = tbl & ~0x7U;
        uint64_t bar = pci_bar_addr(dev, bir);
        if (!bar) {
            kprintf("[pci] MSI-X: BAR%u unavailable, abort\n", (unsigned)bir);
            return false;
        }
        volatile uint8_t *base = (volatile uint8_t *)PHYS_TO_VIRT(bar);
        /* 条目 0：MSG ADDR(8B)@0, MSG DATA(4B)@8, VECTOR CTRL(4B)@12 */
        *(volatile uint32_t *)(base + tbl_off + 0)  = (uint32_t)msi_addr(lapic_id);
        *(volatile uint32_t *)(base + tbl_off + 4)  = 0;
        *(volatile uint32_t *)(base + tbl_off + 8)  = (uint32_t)vector;
        *(volatile uint32_t *)(base + tbl_off + 12) = 0;   /* unmask 条目 0 */
        uint16_t ctrl = pci_cfg_read16(dev->bus, dev->dev, dev->func,
                                       (uint8_t)(off + 2));
        ctrl |= 0x8000;        /* MSI-X Enable */
        ctrl &= ~0x4000;       /* 清 Function Mask */
        pci_cfg_write16(dev->bus, dev->dev, dev->func, (uint8_t)(off + 2), ctrl);
        kprintf("[pci] MSI-X enabled %02x:%02x.%x -> vec %u (lapic %u)\n",
                (unsigned)dev->bus, (unsigned)dev->dev, (unsigned)dev->func,
                (unsigned)vector, (unsigned)lapic_id);
        return true;
    }

    /* —— MSI（cap 0x05） —— */
    if (pci_find_cap(dev, PCI_CAP_MSI, &off)) {
        uint16_t msgctrl = pci_cfg_read16(dev->bus, dev->dev, dev->func,
                                          (uint8_t)(off + 2));
        bool is64 = (msgctrl & 0x80) != 0;       /* bit7：64 位地址能力 */
        pci_cfg_write32(dev->bus, dev->dev, dev->func, (uint8_t)(off + 4),
                        (uint32_t)msi_addr(lapic_id));
        if (is64) {
            pci_cfg_write32(dev->bus, dev->dev, dev->func, (uint8_t)(off + 8), 0);
            pci_cfg_write16(dev->bus, dev->dev, dev->func, (uint8_t)(off + 12),
                            vector);             /* Data 在 cap+12 */
        } else {
            pci_cfg_write16(dev->bus, dev->dev, dev->func, (uint8_t)(off + 8),
                            vector);             /* Data 在 cap+8 */
        }
        msgctrl = pci_cfg_read16(dev->bus, dev->dev, dev->func, (uint8_t)(off + 2));
        msgctrl |= 0x0001;                       /* MSI Enable */
        pci_cfg_write16(dev->bus, dev->dev, dev->func, (uint8_t)(off + 2), msgctrl);
        kprintf("[pci] MSI enabled %02x:%02x.%x -> vec %u (lapic %u)\n",
                (unsigned)dev->bus, (unsigned)dev->dev, (unsigned)dev->func,
                (unsigned)vector, (unsigned)lapic_id);
        return true;
    }
    return false;
}
