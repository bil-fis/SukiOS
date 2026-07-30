/*
 * include/kernel/acpi.h
 * -----------------------------------------------------------------------------
 * ACPI 表解析（最小可用子集）：RSDP 定位 -> XSDT 遍历 -> MADT/FADT/HPET 提取。
 *
 * 这是 P0-1「离开古董机」的物理机拓扑发现入口：MADT 给出 CPU/APIC 枚举、
 * 中断路由（GSI 映射）所需信息；FADT 给出电源/复位；HPET 表给出高精时钟。
 * 设计原则：只解析、只记录，不在此处做设备初始化（初始化在 apic/hpet 驱动）。
 *
 * RSDP 定位（P0-6 更新，三级回退）：
 *   0) Multiboot2 ACPI 标签（tag 14/15）给出的 RSDP 副本——UEFI 启动的
 *      **唯一**可靠来源（UEFI 下 RSDP 在 EFI 配置表任意页，不在传统区域）；
 *   1) 若 EBDA 存在（BIOS 经 BDA 0x40:0x0E 给出段基址），扫描其前 1KiB；
 *   2) 扫描固定范围 0x000E0000..0x000FFFFF（0x10 对齐的 "RSD PTR "）。
 * BIOS 机型上 0) 缺失时 1)/2) 兜底，两种固件形态统一覆盖。
 */
#ifndef _SUKI_KERNEL_ACPI_H
#define _SUKI_KERNEL_ACPI_H

#include <kernel/types.h>

/* 表签名（8 字节） */
#define ACPI_SIG_RSDP  "RSD PTR "
#define ACPI_SIG_XSDT  "XSDT"
#define ACPI_SIG_MADT  "APIC"
#define ACPI_SIG_FADT  "FACP"
#define ACPI_SIG_HPET  "HPET"

/* MADT 子结构类型 */
#define ACPI_MADT_LAPIC      0   /* 处理器本地 APIC */
#define ACPI_MADT_IOAPIC     1   /* I/O APIC */
#define ACPI_MADT_INT_SRC    2   /* 中断源覆盖（ISA 极性/触发修正） */
#define ACPI_MADT_LAPIC_NMI  4   /* 本地 APIC NMI */

#define ACPI_MADT_LAPIC_ENABLED 0x1

/* 解析结果（供 apic/hpet 驱动消费） */
typedef struct acpi_info {
    bool     found;
    uint8_t  rsdp_revision;          /* 0 = ACPI 1.0 (RSDP 指向 RSDT), 2 = 2.0+ (XSDT) */
    uint64_t rsdt_phys;              /* ACPI 1.0：32 位根表 */
    uint64_t xsdt_phys;              /* ACPI 2.0+：64 位根表 */

    /* 直接定位到的关键表物理地址（0 = 未找到） */
    uint64_t madt_phys;
    uint64_t fadt_phys;
    uint64_t hpet_phys;

    /* MADT 枚举摘要 */
    uint32_t lapic_count;            /* 启用的本地 APIC 数 */
    uint32_t madt_lapic_id[256];     /* 各逻辑 CPU 的 LAPIC ID */
    uint64_t ioapic_phys;            /* 第一个 I/O APIC 的 MMIO 基址 */
    uint32_t ioapic_gsi_base;        /* 该 I/O APIC 覆盖的 GSI 起始 */
    uint32_t ioapic_id;

    /* 本地 APIC 寄存器基址（默认 0xFEE00000；可由 MSR 0x1B 覆盖） */
    uint64_t lapic_phys;

    /* FADT 电源管理寄存器（P0-R8：S5 软关机所需；0 表示未解析到）。
     * 偏移严格按 ACPI 规范：PM1a_CNT_BLK @0x3B、PM1b_CNT_BLK @0x3F（32 位
     * I/O 端口，ACPI 1.0/2.0+ 通用；2.0+ 若 32 位为 0 则回落 X_PM1a/X_PM1b
     * GAS @0x90/0x98）。旧实现误用 0x48/0x4C，读到 PM_TMR_BLK/GPE1_BLK 区域，
     * 导致 QEMU 把 0x608 当 PM1b 控制块、向错误端口写 S5 而关机失败。 */
    uint32_t pm1a_cnt_blk;     /* PM1a Control Block I/O 端口 */
    uint32_t pm1b_cnt_blk;     /* PM1b Control Block I/O 端口（多数平台为 0） */
    uint8_t  slp_typ_a;        /* S5 在 PM1a 的 SLP_TYP 值（来自 DSDT _S5 低 3 位） */
    uint8_t  slp_typ_b;        /* S5 在 PM1b 的 SLP_TYP 值 */
    uint64_t dsdt_phys;        /* DSDT 物理地址（FADT @0x28；SLP_TYP 须由此解析） */
} acpi_info_t;

/* 调用 acpi_init() 后读取全局结果（只读共享） */
extern acpi_info_t g_acpi;

/* P0-6：登记 Multiboot2 提供的 RSDP 副本物理地址（kmain 在 acpi_init 之前
 * 调用，传 boot_info.rsdp_copy_phys；0 表示引导器未提供，走传统扫描）。 */
void acpi_set_rsdp_hint(uint64_t rsdp_phys);

/* 定位 RSDP（MB2 提示优先，其次 EBDA/固定区扫描）并遍历 XSDT/RSDT，
 * 填充 g_acpi。返回是否找到 ACPI。 */
bool acpi_init(void);

/* 在已解析的 ACPI 表中查找指定签名（如 "HPET"）的表物理地址；未找到返回 0。 */
uint64_t acpi_find_table(const char sig[8]);

/* P0-R8：ACPI S5 软关机。向 PM1a/PM1b Control Block 写入 SLP_TYP(S5) | SLP_EN
 * 触发平台断电；无 FADT 电源信息则退化为无限停机。 */
void acpi_poweroff(void);

#endif /* _SUKI_KERNEL_ACPI_H */
