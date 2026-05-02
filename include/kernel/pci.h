/* =============================================================================
* SukiOS - PCI 总线接口（传统 CAM + BAR 探测）
 * ============================================================================= */

#ifndef SUKIOS_PCI_H
#define SUKIOS_PCI_H

#include <stdint.h>
#include <stdbool.h>

/* 配置空间 I/O 端口 */
#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

/* 配置空间偏移（示例） */
#define PCI_VENDOR_ID        0x00
#define PCI_DEVICE_ID        0x02
#define PCI_COMMAND          0x04
#define PCI_STATUS           0x06
#define PCI_REVISION_ID      0x08
#define PCI_PROG_IF          0x09
#define PCI_SUBCLASS         0x0A
#define PCI_CLASS            0x0B
#define PCI_HEADER_TYPE      0x0E
#define PCI_BAR0             0x10
/* ... 省略其他，参见 OSDev */

/* BAR 类型 */
#define PCI_BAR_IO           0x1   /* I/O 空间 */
#define PCI_BAR_MEM          0x0   /* 内存映射 */
#define PCI_BAR_PREFETCH     0x8   /* 预取位（内存 BAR 的 bit 3） */
#define PCI_BAR_64BIT        0x4   /* 64 位地址（bit 2） */

/* 设备信息 */
typedef struct {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  header_type;

    /* BAR 信息 */
    uint32_t bar[6];         /* 原始值 */
    uint64_t bar_addr[6];    /* 实际物理地址（I/O 或 MMIO） */
    uint32_t bar_size[6];    /* 字节大小（0 表示未使用） */
    uint8_t  bar_type[6];    /* PCI_BAR_IO / PCI_BAR_MEM */
} pci_device_t;

/* 接口 */
void pci_init(void);
void pci_enumerate(void);
uint32_t pci_config_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
bool     pci_device_exists(uint8_t bus, uint8_t slot, uint8_t func);

#endif