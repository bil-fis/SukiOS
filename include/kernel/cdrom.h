/*
 * include/kernel/cdrom.h
 * -----------------------------------------------------------------------------
 * 内核态 ATAPI CD-ROM 读取接口（仅 PIO 轮询，无中断依赖）。
 *
 * 设计依据（OSDev《ATAPI》与《ATA/ATAPI using DMA/Packet》）：
 *   - QEMU `-cdrom` 默认把光盘挂到 Secondary Master（IDE 总线 1，单元 0），
 *     传统 I/O 基址 0x170、Alternate Status/Control 0x376、IRQ 15。
 *   - 这是唯一驻留 Ring0 的光盘读取通道；内核 ISO9660 驱动（kernel/fs/iso9660.c）
 *     经本接口按 2048 字节逻辑块读盘，不解析任何文件系统结构（红线：格式解析
 *     在内核 iso9660.c，块设备原始读在 cdrom.c，二者严格分层）。
 *   - 仅支持 READ(10) 数据包命令；写/音频等命令不在内核需求内（只读 OS 镜像）。
 */
#ifndef _SUKI_KERNEL_CDROM_H
#define _SUKI_KERNEL_CDROM_H

#include <stdint.h>
#include <stdbool.h>

/* 初始化并探测 ATAPI 光驱。成功检测到设备返回 true（g_cdrom_present 置位）。 */
bool CdromInit(void);

/* 光驱是否就绪（探测成功且介质可读）。 */
bool CdromPresent(void);

/* 从逻辑块地址 lba 起读 count 个 2048 字节块到 buf（buf 须 >= count*2048）。
 * 成功返回 true，失败（命令错误/超时/越界）返回 false。 */
bool CdromReadBlocks(uint32_t lba, uint32_t count, void *buf);

#endif /* _SUKI_KERNEL_CDROM_H */
