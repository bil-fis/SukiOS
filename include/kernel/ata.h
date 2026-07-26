/*
 * include/kernel/ata.h
 * -----------------------------------------------------------------------------
 * ATA PIO 磁盘驱动（LBA28，Primary Master）。
 *
 * 红线（手册 3 章）：磁盘裸读写是唯一驻留内核态的驱动，仅通过
 * DISK_PORT 响应 IPC 请求对外服务；文件系统解析在 Ring3 FS_SERVER。
 */
#ifndef _SUKI_KERNEL_ATA_H
#define _SUKI_KERNEL_ATA_H

#include <kernel/types.h>

#define ATA_SECTOR_SIZE 512

/* 探测 Primary Master；返回是否存在可用磁盘 */
bool ata_init(void);

/* PIO 读 count 个扇区到 buf（须容纳 count*512 字节）；成功返回 true */
bool ata_read_sectors(uint32_t lba, uint8_t count, void *buf);

/* PIO 写 count 个扇区（写后执行 FLUSH CACHE 确保落盘）；成功返回 true */
bool ata_write_sectors(uint32_t lba, uint8_t count, const void *buf);

/* 磁盘总扇区数（IDENTIFY 报告的 LBA28 容量） */
uint32_t ata_total_sectors(void);

/* 启动内核磁盘服务任务（拥有 DISK_PORT，处理 IPC 读请求） */
void disk_srv_start(void);

#endif /* _SUKI_KERNEL_ATA_H */
