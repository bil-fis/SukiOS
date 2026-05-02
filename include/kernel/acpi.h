/* =============================================================================
 * SukiOS - ACPI 表结构定义
 *
 * 参考：OSDev MADT, ACPI Specification 6.5 Section 5.2.12
 * ============================================================================= */

#ifndef SUKIOS_ACPI_H
#define SUKIOS_ACPI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---- 通用 SDT 表头（所有 ACPI 表共有） ---- */
struct acpi_sdt_header {
    char     signature[4];      /* 表签名，如 "APIC" */
    uint32_t length;            /* 整表长度（含表头） */
    uint8_t  revision;          /* ACPI 规范版本 */
    uint8_t  checksum;          /* 校验和 */
    char     oem_id[6];         /* OEM 标识 */
    char     oem_table_id[8];   /* OEM 表标识 */
    uint32_t oem_revision;      /* OEM 修订号 */
    uint32_t creator_id;        /* 创建者 ID */
    uint32_t creator_revision;  /* 创建者修订号 */
} __attribute__((packed));

/* ---- RSDP（Root System Description Pointer） ---- */
struct acpi_rsdp {
    char     signature[8];      /* "RSD PTR " */
    uint8_t  checksum;          /* 前 20 字节校验和 */
    char     oem_id[6];         /* OEM 标识 */
    uint8_t  revision;          /* 0 = ACPI 1.0, 2+ = ACPI 2.0+ */
    uint32_t rsdt_addr;         /* RSDT 物理地址 */
    /* 以下字段仅 ACPI 2.0+ 有效 */
    uint32_t length;            /* RSDP 总长度 */
    uint64_t xsdt_addr;         /* XSDT 物理地址（64 位） */
    uint8_t  ext_checksum;      /* 扩展校验和（全部字段） */
    uint8_t  reserved[3];
} __attribute__((packed));

/* ---- MADT（Multiple APIC Description Table） ---- */
struct acpi_madt {
    struct acpi_sdt_header header;
    uint32_t local_apic_addr;   /* Local APIC 基地址（32 位） */
    uint32_t flags;             /* bit 0 = 双 8259 PIC 已安装 */
} __attribute__((packed));

/* ---- MADT 条目公共头 ---- */
struct acpi_madt_entry {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

/* ---- MADT 类型 1: I/O APIC ---- */
struct acpi_madt_ioapic {
    uint8_t  type;              /* 1 */
    uint8_t  length;            /* 12 */
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_addr;       /* I/O APIC MMIO 基地址 */
    uint32_t gsi_base;          /* 全局系统中断基号 */
} __attribute__((packed));

/* ---- MADT 类型 5: Local APIC 地址覆盖（64 位系统） ---- */
struct acpi_madt_lapic_override {
    uint8_t  type;              /* 5 */
    uint8_t  length;            /* 12 */
    uint16_t reserved;
    uint64_t lapic_addr;        /* 64 位 Local APIC 地址 */
} __attribute__((packed));

/* ---- MADT 类型 2: 中断源覆盖 ---- */
struct acpi_madt_int_override {
    uint8_t  type;              /* 2 */
    uint8_t  length;            /* 10 */
    uint8_t  bus;               /* 0 = ISA */
    uint8_t  irq;               /* 源 IRQ */
    uint32_t gsi;               /* 全局系统中断号 */
    uint16_t flags;
} __attribute__((packed));

/* ---- MADT 解析结果 ---- */
struct acpi_madt_info {
    uint32_t lapic_addr;        /* Local APIC 基地址 */
    uint32_t flags;
    bool     ioapic_found;
    uint8_t  ioapic_id;
    uint32_t ioapic_addr;       /* I/O APIC MMIO 基地址 */
    uint32_t ioapic_gsi_base;
};

/* ---- ISA 中断源覆盖查找 ---- */
#define ACPI_MAX_ISA_OVERRIDES 16

/* 查找指定 ISA IRQ 的 GSI 和 flags
 * @isa_irq: ISA IRQ 编号 (0-15)
 * @gsi:     输出 GSI (IOAPIC 引脚号)
 * @flags:   输出 MADT flags (极性/触发模式)
 * @return:  true 表示找到 override，false 表示使用默认映射 (GSI=isa_irq, flags=0) */
bool acpi_get_isa_override(uint8_t isa_irq, uint32_t *gsi, uint16_t *flags);

/* 初始化 ISA override 表（在 apic_init 中调用） */
void acpi_parse_isa_overrides(struct acpi_sdt_header *madt_hdr);

/* ---- 函数声明 ---- */

/* 在 BIOS 区域搜索 RSDP */
struct acpi_rsdp *acpi_find_rsdp(void);

/* 在 XSDT/RSDT 中查找指定签名的 ACPI 表 */
struct acpi_sdt_header *acpi_find_table(struct acpi_rsdp *rsdp, const char *signature);

/* 解析 MADT 表，提取 APIC 信息 */
struct acpi_madt_info acpi_parse_madt(struct acpi_sdt_header *madt_hdr);

#endif /* SUKIOS_ACPI_H */
