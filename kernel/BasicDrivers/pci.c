/* =============================================================================
 * SukiOS - PCI 枚举（传统 CAM）+ BAR 探测
 * 参考：https://wiki.osdev.org/PCI
 * ============================================================================= */

#include "kernel/pci.h"
#include "kernel/tty.h"
#include "kernel/io.h"
#include "kernel/vmm.h"

/* ---- 配置空间访问 ---- */
uint32_t pci_config_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t address = (1U << 31)
                     | ((uint32_t)bus      << 16)
                     | ((uint32_t)slot     << 11)
                     | ((uint32_t)func     <<  8)
                     | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    io_wait();
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value)
{
    uint32_t address = (1U << 31)
                     | ((uint32_t)bus      << 16)
                     | ((uint32_t)slot     << 11)
                     | ((uint32_t)func     <<  8)
                     | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    io_wait();
    outl(PCI_CONFIG_DATA, value);
}

bool pci_device_exists(uint8_t bus, uint8_t slot, uint8_t func)
{
    uint16_t vendor = (uint16_t)(pci_config_read(bus, slot, func, PCI_VENDOR_ID) & 0xFFFF);
    return (vendor != 0xFFFF);
}

/* ---- Class Code 名称表 ---- */
static const char *class_names[18] = {
    [0x00] = "Legacy",
    [0x01] = "Mass Storage",
    [0x02] = "Network",
    [0x03] = "Display",
    [0x04] = "Multimedia",
    [0x05] = "Memory",
    [0x06] = "Bridge",
    [0x07] = "Communication",
    [0x08] = "Base System Periph.",
    [0x09] = "Input",
    [0x0A] = "Docking",
    [0x0B] = "Processor",
    [0x0C] = "Serial Bus",
    [0x0D] = "Wireless",
    [0x0E] = "Intelligent IO",
    [0x0F] = "Satellite",
    [0x10] = "Encryption",
    [0x11] = "Data Acquisition",
};

/* ---- BAR 探测 ---- */
static void pci_probe_bar(pci_device_t *dev, int bar_index)
{
    uint32_t bar_val = dev->bar[bar_index];
    uint32_t bar_orig = bar_val;

    /* 写入全1探测 */
    pci_config_write(dev->bus, dev->slot, dev->function,
                     PCI_BAR0 + bar_index * 4, 0xFFFFFFFF);
    uint32_t bar_resp = pci_config_read(dev->bus, dev->slot, dev->function,
                                        PCI_BAR0 + bar_index * 4);
    /* 恢复原值 */
    pci_config_write(dev->bus, dev->slot, dev->function,
                     PCI_BAR0 + bar_index * 4, bar_orig);

    if (bar_resp == 0 || bar_resp == 0xFFFFFFFF) {
        dev->bar_size[bar_index] = 0;
        return;
    }

    if (bar_val & PCI_BAR_IO) {
        /* I/O 空间 BAR */
        dev->bar_type[bar_index] = PCI_BAR_IO;
        dev->bar_addr[bar_index] = bar_val & 0xFFFFFFFC;
        uint32_t size = ~(bar_resp & 0xFFFFFFFC) + 1;
        dev->bar_size[bar_index] = size & 0xFFFFFFFF;
    } else {
        /* 内存映射 BAR */
        dev->bar_type[bar_index] = PCI_BAR_MEM;
        /* 检查 64 位 bit (bit 2)，但我们目前简化为 32 位地址 */
        dev->bar_addr[bar_index] = bar_val & 0xFFFFFFF0;
        uint32_t size = ~(bar_resp & 0xFFFFFFF0) + 1;
        dev->bar_size[bar_index] = size;
    }
}

/* ---- 打印设备信息 ---- */
static void pci_print_device(const pci_device_t *dev)
{
    tty_setcolor(VGA_LIGHT_GREY, VGA_BLACK);

    /* 总线:设备.功能 */
    tty_print_dec(dev->bus);
    tty_print(":");
    tty_print_dec(dev->slot);
    tty_print(".");
    tty_print_dec(dev->function);

    /* Vendor/Device */
    tty_print("  ");
    tty_print_hex64((uint64_t)dev->vendor_id);
    tty_print(":");
    tty_print_hex64((uint64_t)dev->device_id);

    /* Class */
    tty_print("  Class ");
    tty_print_hex64((uint64_t)dev->class_code);
    tty_print(".");
    tty_print_hex64((uint64_t)dev->subclass);
    tty_print(".");
    tty_print_hex64((uint64_t)dev->prog_if);

    const char *class_name = (dev->class_code < 18) ?
                             class_names[dev->class_code] : "Unknown";
    tty_print(" (");
    tty_print(class_name);
    tty_print(")\n");

    /* 打印有效的 BAR */
    for (int i = 0; i < 6; ++i) {
        if (dev->bar_size[i] == 0) continue;

        tty_print("      BAR");
        tty_print_dec(i);
        tty_print(": ");
        if (dev->bar_type[i] == PCI_BAR_IO) {
            tty_print("I/O  ");
            tty_print_hex64(dev->bar_addr[i]);
        } else {
            tty_print("MMIO ");
            tty_print_hex64(dev->bar_addr[i]);
        }
        tty_print(" size=");
        tty_print_hex64((uint64_t)dev->bar_size[i]);
        tty_print(" bytes\n");
    }
}

/* ---- 枚举主循环 ---- */
void pci_enumerate(void)
{
    tty_print("[..] Enumerating PCI bus...\n");

    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            if (!pci_device_exists((uint8_t)bus, slot, 0))
                continue;

            pci_device_t dev;
            dev.bus    = (uint8_t)bus;
            dev.slot   = slot;
            dev.function = 0;

            uint32_t id_reg = pci_config_read(dev.bus, dev.slot, 0, 0x00);
            dev.vendor_id = (uint16_t)(id_reg & 0xFFFF);
            dev.device_id = (uint16_t)(id_reg >> 16);

            uint32_t class_reg = pci_config_read(dev.bus, dev.slot, 0, 0x08);
            dev.class_code = (class_reg >> 24) & 0xFF;
            dev.subclass   = (class_reg >> 16) & 0xFF;
            dev.prog_if    = (class_reg >> 8) & 0xFF;

            uint32_t header_reg = pci_config_read(dev.bus, dev.slot, 0, 0x0C);
            dev.header_type = (header_reg >> 16) & 0x7F;
            bool multifunc = (header_reg & 0x800000) != 0;  /* 多功能标志 */

            /* 读取 BAR */
            for (int i = 0; i < 6; ++i) {
                dev.bar[i] = pci_config_read(dev.bus, dev.slot, 0, PCI_BAR0 + i * 4);
                pci_probe_bar(&dev, i);
            }

            pci_print_device(&dev);

            /* 多功能设备 */
            if (multifunc) {
                for (uint8_t func = 1; func < 8; ++func) {
                    if (!pci_device_exists(dev.bus, slot, func))
                        continue;

                    dev.function = func;
                    id_reg = pci_config_read(dev.bus, dev.slot, func, 0x00);
                    dev.vendor_id = (uint16_t)(id_reg & 0xFFFF);
                    dev.device_id = (uint16_t)(id_reg >> 16);

                    class_reg = pci_config_read(dev.bus, dev.slot, func, 0x08);
                    dev.class_code = (class_reg >> 24) & 0xFF;
                    dev.subclass   = (class_reg >> 16) & 0xFF;
                    dev.prog_if    = (class_reg >> 8) & 0xFF;

                    header_reg = pci_config_read(dev.bus, dev.slot, func, 0x0C);
                    dev.header_type = (header_reg >> 16) & 0x7F;
                    for (int i = 0; i < 6; ++i) {
                        dev.bar[i] = pci_config_read(dev.bus, dev.slot, func, PCI_BAR0 + i * 4);
                        pci_probe_bar(&dev, i);
                    }
                    pci_print_device(&dev);
                }
            }
        }
    }

    tty_print("[OK] PCI enumeration complete.\n");
}

void pci_init(void)
{
    pci_enumerate();
}