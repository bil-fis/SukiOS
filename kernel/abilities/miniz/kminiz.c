/*
 * kernel/abilities/miniz/kminiz.c
 * -----------------------------------------------------------------------------
 * SukiOS 内核内嵌压缩能力（Ring0）实现。
 *
 * 组成：
 *   1) 宿主垫片：为 miniz 提供 malloc/calloc/realloc/free（→ 内核堆）与 assert 失败钩子；
 *   2) 内核 API：kminiz_compress/uncompress/... 包装 miniz 的 zlib 风格接口；
 *   3) 开机自检：kminiz_selftest()，压缩→解压往返一致性（由 kmain 调用）。
 *
 * miniz 源码（kernel/abilities/miniz/miniz.c）以本文件同款宏配置单独编译进内核。
 * 该能力为后续最小系统环境（minOSEnv / rootfs，如 initramfs、.spkg 解压）预留。
 */
#include <stddef.h>
#include <stdint.h>
#include <kernel/types.h>
#include <kernel/string.h>
#include <kernel/console.h>
#include <mm/kmalloc.h>
#include <kernel/abilities/kminiz.h>

/* ---- miniz 配置（必须与 miniz.c 编译宏一致） ----
 * 用 #ifndef 守卫：Makefile 的 KMINIZ_CFLAGS 已通过 -D 传入这些宏，
 * 此处再定义会触发 -Wmacro-redefined；守卫后两者不冲突，本文件仍提供兜底。 */
#ifndef MINIZ_NO_STDIO
#define MINIZ_NO_STDIO
#endif
#ifndef MINIZ_NO_TIME
#define MINIZ_NO_TIME
#endif
#ifndef MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ARCHIVE_APIS
#endif
#ifndef MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#endif

#include <kernel/abilities/miniz/miniz.h>
#include <stdlib.h>      /* miniz 垫片：仅声明 malloc/calloc/realloc/free */
#include <string.h>      /* miniz 垫片：内核字符串库 + memchr */
#include <assert.h>      /* miniz 垫片：assert 钩子 */

/* ============================================================================
 * 1) 宿主垫片：miniz 的分配接口 → 内核堆
 * ========================================================================= */
void *malloc(size_t n)
{
    return kmalloc(n);
}

void *calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    if (size && total / size != nmemb) {   /* 溢出保护 */
        return NULL;
    }
    return kzalloc(total);
}

void *realloc(void *ptr, size_t n)
{
    return krealloc(ptr, n);
}

void free(void *ptr)
{
    kfree(ptr);
}

/* miniz 的 MZ_ASSERT 失败钩子：打印告警但继续（内核不因压缩库断言而 panic）。 */
void __kminiz_assert_fail(const char *expr, const char *file, int line)
{
    kprintf("[kminiz] assert failed: %s (%s:%d)\n", expr, file, line);
}

/* 内核字符串库未提供 memchr；miniz 可能引用，此处补齐。 */
void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned char ch = (unsigned char)c;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == ch) {
            return (void *)(uintptr_t)(p + i);
        }
    }
    return NULL;
}

/* ============================================================================
 * 2) 内核压缩 API（包装 miniz）
 * ========================================================================= */
int kminiz_compress(void *dst, size_t *dst_len,
                    const void *src, size_t src_len, int level)
{
    if (!dst || !dst_len || !src) {
        return -1;
    }
    mz_ulong dl = (mz_ulong)*dst_len;
    int rc = mz_compress2((unsigned char *)dst, &dl,
                          (const unsigned char *)src, (mz_ulong)src_len, level);
    if (rc == MZ_OK) {
        *dst_len = (size_t)dl;
    }
    return rc;
}

int kminiz_uncompress(void *dst, size_t *dst_len,
                      const void *src, size_t src_len)
{
    if (!dst || !dst_len || !src) {
        return -1;
    }
    mz_ulong dl = (mz_ulong)*dst_len;
    int rc = mz_uncompress((unsigned char *)dst, &dl,
                           (const unsigned char *)src, (mz_ulong)src_len);
    if (rc == MZ_OK) {
        *dst_len = (size_t)dl;
    }
    return rc;
}

size_t kminiz_compress_bound(size_t src_len)
{
    return (size_t)mz_compressBound((mz_ulong)src_len);
}

uint32_t kminiz_adler32(uint32_t adler, const void *buf, size_t len)
{
    return (uint32_t)mz_adler32((mz_ulong)adler, (const unsigned char *)buf, len);
}

uint32_t kminiz_crc32(uint32_t crc, const void *buf, size_t len)
{
    return (uint32_t)mz_crc32((mz_ulong)crc, (const unsigned char *)buf, len);
}

/* ============================================================================
 * 3) 开机自检
 * ========================================================================= */
int kminiz_selftest(void)
{
    static const char msg[] =
        "SukiOS kernel miniz capability round trip: "
        "The quick brown fox jumps over the lazy dog. 0123456789 0123456789";
    size_t slen = strlen(msg) + 1;

    uint8_t comp[256];
    size_t clen = sizeof(comp);
    int rc = kminiz_compress(comp, &clen, msg, slen, MZ_DEFAULT_LEVEL);
    if (rc != MZ_OK) {
        kprintf("[kminiz] selftest FAIL: compress rc=%d\n", rc);
        return -1;
    }

    uint8_t decomp[256];
    size_t dlen = sizeof(decomp);
    rc = kminiz_uncompress(decomp, &dlen, comp, clen);
    if (rc != MZ_OK || dlen != slen || memcmp(decomp, msg, slen) != 0) {
        kprintf("[kminiz] selftest FAIL: uncompress rc=%d dlen=%lu slen=%lu\n",
                rc, (unsigned long)dlen, (unsigned long)slen);
        return -2;
    }

    kprintf("[kminiz] selftest OK (%lu -> %lu bytes, zlib-style roundtrip)\n",
            (unsigned long)slen, (unsigned long)clen);
    return 0;
}
