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

#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

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
    outl(PCI_CONFIG_ADDRESS, cfg_addr(bus, dev, func, off));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_cfg_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    uint32_t v = pci_cfg_read32(bus, dev, func, off);
    return (uint16_t)(v >> ((off & 2) * 8));
}

uint8_t pci_cfg_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    uint32_t v = pci_cfg_read32(bus, dev, func, off);
    return (uint8_t)(v >> ((off & 3) * 8));
}

void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off,
                     uint32_t val)
{
    outl(PCI_CONFIG_ADDRESS, cfg_addr(bus, dev, func, off));
    outl(PCI_CONFIG_DATA, val);
}

void pci_cfg_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off,
                     uint16_t val)
{
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
