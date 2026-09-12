/*
 * include/kernel/abilities/kminiz.h
 * -----------------------------------------------------------------------------
 * SukiOS 内核内嵌压缩能力（Ring0）对外 API。
 *
 * 内核中嵌入 miniz（zlib 风格编解码），为后续最小系统环境（minOSEnv / rootfs）
 * 提供内核态压缩/解压能力（例如解压 initramfs、.spkg 软件包等）。
 *
 * 实现在 kernel/abilities/miniz/kminiz.c；分配经内核堆（kmalloc/kzalloc/krealloc/kfree）。
 */
#ifndef _SUKI_KMINIZ_H
#define _SUKI_KMINIZ_H

#include <stddef.h>
#include <stdint.h>

/* 返回码：0 成功（对应 miniz 的 MZ_OK）；负数为 miniz 错误码。 */
#define KMINIZ_OK 0

/* 单次压缩：dst/dst_len 既作输入（缓冲容量）又作输出（实际长度）。
 * level 取 0..10，常用 6（MZ_DEFAULT_LEVEL）。 */
int    kminiz_compress(void *dst, size_t *dst_len,
                       const void *src, size_t src_len, int level);

/* 单次解压：dst/dst_len 同上级语义。 */
int    kminiz_uncompress(void *dst, size_t *dst_len,
                         const void *src, size_t src_len);

/* 压缩输出上界（用于分配 dst 缓冲）。 */
size_t kminiz_compress_bound(size_t src_len);

/* 校验和（供校验用）。 */
uint32_t kminiz_adler32(uint32_t adler, const void *buf, size_t len);
uint32_t kminiz_crc32(uint32_t crc, const void *buf, size_t len);

/* 开机自检：压缩→解压往返一致性校验；返回 0 成功。 */
int    kminiz_selftest(void);

#endif /* _SUKI_KMINIZ_H */
