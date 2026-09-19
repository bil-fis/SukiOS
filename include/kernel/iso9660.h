/*
 * include/kernel/iso9660.h
 * -----------------------------------------------------------------------------
 * 内核内嵌 ISO9660 文件系统驱动接口（只读，支持 Rock Ridge 与 Joliet 扩展）。
 *
 * 设计依据（OSDev《ISO 9660》/《Rock Ridge》/《Joliet》）：
 *   - 本驱动驻留 Ring0，是对「内核不解析文件系统」红线的显式例外——它使内核
 *     能脱离硬盘、仅从自身启动用的 ISO 光盘（ATAPI CD-ROM）读取全部系统文件，
 *     支撑 `make run-single-iso` 的「光盘即系统盘」场景。
 *   - 块读取经 kernel/drivers/cdrom.c（ATAPI PACKET/READ(10)），本文件只做
 *     卷/目录/目录项解析与路径解析（namei）。
 *   - 扩展支持：
 *       * Rock Ridge (RRIP/SUSP)：目录项系统使用区解析 "NM"(真实文件名) 与
 *         "PX"(POSIX 模式/权限)，大小写与权限完整保留；
 *       * Joliet：补充卷描述符(SVD, type=2) 的 UCS-2 文件名，当卷同时无
 *         Rock Ridge 时自动启用，提供长名/Unicode 文件名。
 *   - 卷描述符序列：LBA 16 起，type=1 主卷(PVD)，type=2 补充卷(Joliet 候选)，
 *     type=0 终止符。卷大小、逻辑块大小、根目录记录均取自 PVD。
 */
#ifndef _SUKI_KERNEL_ISO9660_H
#define _SUKI_KERNEL_ISO9660_H

#include <stdint.h>
#include <stdbool.h>
#include <ipc/fs_proto.h>   /* fs_stat_t / fs_dirent_t */

/* 只读挂载 CD-ROM 上的 ISO9660 卷。成功返回 true（g_iso_mounted 置位）。
 * 要求 CdromInit() 已成功（光驱已探测）。 */
bool IsoMount(void);

/* 是否已成功挂载 ISO 卷（供 VFS 层决定 '/' 后端）。 */
bool IsoIsMounted(void);

/* 内核态按路径读整个文件（供启动期读 registry 等小文件），成功返回 0，
 * *out_n 为读取字节；失败返回负 errno。buffer 由调用方提供（容量 cap）。 */
int IsoReadFile(const char *path, uint8_t *buf, uint32_t cap, uint32_t *out_n);

/* ---- 句柄式文件操作（供 fd.c / vfs.c 直接调用，不经 IPC，零拷贝内核态）---- */
int  iso_open(const char *path, int32_t flags, uint32_t mode, uint32_t *out_handle);
int  iso_read(uint32_t h, void *buf, uint32_t len, uint64_t *out_nread);
int  iso_lseek(uint32_t h, int64_t offset, int whence, uint64_t *out_pos);
int  iso_close(uint32_t h);
int  iso_stat(const char *path, fs_stat_t *st);
int  iso_opendir(const char *path);
int  iso_readdir(int dd, fs_dirent_t *de);
int  iso_closedir(int dd);

/* 启动自测：从 ISO 根读一个已知小文件并打印内容 + 列出根目录条目，
 * 验证「块设备 -> ISO 解析 -> 文件内容」全链路（生产场景验证）。 */
void IsoSelfTest(void);

#endif /* _SUKI_KERNEL_ISO9660_H */
