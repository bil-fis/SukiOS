/*
 * include/kernel/pci.h
 * -----------------------------------------------------------------------------
 * PCI 配置空间访问（传统机制 #1：I/O 端口 0xCF8 CONFIG_ADDRESS / 0xCFC
 * CONFIG_DATA）。-machine pc (i440FX) 下所有设备（含 intel-hda）均可通过
 * 该机制枚举，无需 MMCONFIG/ACPI。
 *
 * 调用关系：kmain -> hda_init() -> pci_find_class() / pci_cfg_read*()。
 */
#ifndef _SUKI_KERNEL_PCI_H
#define _SUKI_KERNEL_PCI_H

#include <kernel/types.h>

/* PCI 配置空间通用寄存器偏移 */
#define PCI_CFG_VENDOR_ID    0x00   /* u16 */
#define PCI_CFG_DEVICE_ID    0x02   /* u16 */
#define PCI_CFG_COMMAND      0x04   /* u16 */
#define PCI_CFG_STATUS       0x06   /* u16 */
#define PCI_CFG_REVISION     0x08   /* u8  */
#define PCI_CFG_PROG_IF      0x09   /* u8  */
#define PCI_CFG_SUBCLASS     0x0A   /* u8  */
#define PCI_CFG_CLASS        0x0B   /* u8  */
#define PCI_CFG_HEADER_TYPE  0x0E   /* u8  */
#define PCI_CFG_BAR0         0x10   /* u32 */
#define PCI_CFG_INT_LINE     0x3C   /* u8  */
#define PCI_CFG_INT_PIN      0x3D   /* u8  (1=A..4=D, 0=无中断) */
#define PCI_CFG_CAP_PTR      0x34   /* u8  能力链表头指针 */

/* COMMAND 寄存器位 */
#define PCI_CMD_IO_SPACE     (1 << 0)
#define PCI_CMD_MEM_SPACE    (1 << 1)
#define PCI_CMD_BUS_MASTER   (1 << 2)

/* 设备定位（bus/device/function 三元组） */
typedef struct pci_dev {
    uint8_t bus;
    uint8_t dev;
    uint8_t func;
    uint16_t vendor_id;
    uint16_t device_id;
} pci_dev_t;

uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
uint16_t pci_cfg_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
uint8_t  pci_cfg_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
void     pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off,
                         uint32_t val);
void     pci_cfg_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off,
                         uint16_t val);

/* 按 (class, subclass) 扫描全部总线，找到第一个匹配设备。
 * 成功返回 true 并填充 *out；未找到返回 false。 */
bool pci_find_class(uint8_t class_code, uint8_t subclass, pci_dev_t *out);

/* 读取 BAR（返回屏蔽低位标志后的物理基址；仅支持 32/64 位 MMIO BAR）。
 * bar_index 0..5；I/O BAR 或未实现返回 0。 */
uint64_t pci_bar_addr(const pci_dev_t *d, uint8_t bar_index);

/* 使能 MMIO 访问 + 总线主控（DMA），HDA 控制器必需 */
void pci_enable_device(const pci_dev_t *d);

/* P0-1：若 MCFG 提供 ECAM 窗口，则把 PCI 配置访问切换到 MMIO ECAM
 *（更快、可覆盖 PCIe 扩展配置空间 0..4095）；否则保持传统 PIO 0xCF8。
 * 须在 acpi_init() 之后、任何配置访问之前调用一次。 */
void pci_cfg_init(void);

/* P0-1/R8：解析 PCI 设备 INTx#（int_pin 为配置空间 0x3D 的值 1..4）的 GSI。
 * 优先用 _PRT（acpi_pci_route），失败则回落固件预编程的 INT_LINE(0x3C)。
 * 返回 GSI（>=0）；无中断能力（int_pin==0）返回 -1。 */
int pci_route_interrupt(const pci_dev_t *dev, uint8_t int_pin);

/* P0-2/R6：MSI/MSI-X 能力发现与编程。
 *  pci_find_cap   : 遍历能力链表查找 cap_id（0x05=MSI, 0x11=MSI-X）；命中时
 *                   *off_out 返回能力寄存器在配置空间中的偏移。
 *  pci_msi_capable: 探测设备是否支持 MSI/MSI-X（has_msix 输出是否支持 MSI-X）。
 *  pci_enable_msi : 优先 MSI-X，否则 MSI；把中断投递到 (lapic_id, vector)。
 *                   成功返回 true（调用方改用 MSI 向量、不再配置 IOAPIC）；
 *                   设备不支持或编程失败返回 false（调用方回落 IOAPIC）。 */
bool pci_find_cap(const pci_dev_t *dev, uint8_t cap_id, uint8_t *off_out);
bool pci_msi_capable(const pci_dev_t *dev, bool *has_msix);
bool pci_enable_msi(const pci_dev_t *dev, uint8_t vector, uint8_t lapic_id);

/* MSI/MSI-X 能力 ID（PCI 规范） */
#define PCI_CAP_MSI   0x05
#define PCI_CAP_MSIX  0x11

#endif /* _SUKI_KERNEL_PCI_H */
