/*
 * include/kernel/ioapic.h
 * -----------------------------------------------------------------------------
 * I/O APIC 驱动（x86 标准 MMIO）。P0-2 一部分：取代 8259 PIC 做外部中断路由。
 *
 * 设计：I/O APIC 把全局系统中断 (GSI) 路由到某个 LAPIC（含向量号、触发方式）。
 * 本项目（单 BSP、i440FX/PIIX3）GSI 与 ISA IRQ 身份映射（IRQ1=键盘→GSI1），
 * 故 ioapic_route(gsi, vector, ...) 即可。MSI/MSI-X（P0-2 收尾）走 PCI 设备
 * 直接投送，不经由此处 GSI 表。
 */
#ifndef _SUKI_KERNEL_IOAPIC_H
#define _SUKI_KERNEL_IOAPIC_H

#include <kernel/types.h>

/* 初始化 I/O APIC：定位 MMIO 基址（来自 ACPI MADT，默认 0xFEC00000），
 * 屏蔽全部重定向条目（避免 BIOS 残留误触发），返回是否成功。 */
bool ioapic_init(void);

/* 将 GSI 路由到指定 LAPIC 向量。
 *   gsi        : 全局系统中断号（ISA 下 == IRQ 号）
 *   vector     : IDT 向量号（如 IRQ1=33）
 *   level      : true=电平触发, false=边沿触发
 *   active_low : true=低电平/下降沿有效, false=高电平/上升沿有效
 *   dest       : 目标 LAPIC ID（默认 BSP 的 LAPIC ID，单核=0） */
void ioapic_route(uint32_t gsi, uint8_t vector,
                  bool level, bool active_low, uint8_t dest);

/* 设置后续 ioapic_route 的默认目标 LAPIC ID（BSP 的 LAPIC ID，单核=0）。 */
void ioapic_set_dest(uint8_t lapic_id);

/* 屏蔽/放开某 GSI（写重定向条目 mask 位） */
void ioapic_mask(uint32_t gsi);
void ioapic_unmask(uint32_t gsi);

#endif /* _SUKI_KERNEL_IOAPIC_H */
