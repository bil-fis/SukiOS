/*
 * include/kernel/ahci.h
 * -----------------------------------------------------------------------------
 * AHCI SATA 驱动（P0-7：中断驱动 DMA，取代 ATA PIO 的现代路径）。
 *
 * 红线（手册 3 章）：磁盘裸读写是唯一驻留内核态的驱动，仅通过 DISK_PORT
 * 响应 IPC 请求对外服务；文件系统解析在 Ring3 FS_SERVER。本驱动与 ata.c
 * （PIO 回退路径）由 disk-srv 统一分发：AHCI 控制器存在则优先走 DMA。
 *
 * 模型：
 *   - PCI class 0x01 / subclass 0x06 (SATA AHCI)，ABAR = BAR5（MMIO）；
 *   - 单端口单命令槽（slot 0）驱动：命令列表/接收 FIS/命令表各占页；
 *   - 数据经 4KB 弹跳页 DMA（DISK_MAX_SECTORS=7 扇区=3.5KB < 1 页），
 *     规避内核堆虚拟连续/物理不连续问题；
 *   - 完成通知：PxIE+GHC.IE 使能，PCI INTx 经 IOAPIC 路由（GSI=INT_LINE，
 *     电平低有效），IRQ 处理器置完成标志；主流程等待标志，同时轮询
 *     PxCI 兜底（中断丢失也能完成，只慢不坏）。
 *
 * 调用关系：kmain -> ahci_init()；
 *           ata.c::disk_srv_task -> ahci_present() ? ahci_* : ata_*。
 */
#ifndef _SUKI_KERNEL_AHCI_H
#define _SUKI_KERNEL_AHCI_H

#include <kernel/types.h>

/* 探测并初始化第一个挂盘的 AHCI 端口；无控制器/无盘返回 false */
bool ahci_init(void);

/* 是否有可用的 AHCI 盘（disk-srv 分发依据） */
bool ahci_present(void);

/* DMA 读/写扇区（单次 ≤ 8 扇区，弹跳页限制）；成功返回 true。
 * 写路径带 FLUSH（发 DMA 写后不缓存——QEMU/AHCI 写完成即落虚拟盘）。 */
bool ahci_read_sectors(uint64_t lba, uint32_t count, void *buf);
bool ahci_write_sectors(uint64_t lba, uint32_t count, const void *buf);

/* 磁盘总扇区数（IDENTIFY word100-103 LBA48 优先，回退 word60-61） */
uint64_t ahci_total_sectors(void);

#endif /* _SUKI_KERNEL_AHCI_H */
