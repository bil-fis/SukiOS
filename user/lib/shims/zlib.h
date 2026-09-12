/*
 * user/lib/shims/zlib.h
 * -----------------------------------------------------------------------------
 * SukiOS 用户态 zlib 兼容头：miniz 以 zlib 兼容模式提供标准 zlib 符号
 * （inflate/deflate/compress/uncompress/z_stream/zlibVersion 等）。
 * 供 libcurl 以 HAVE_ZLIB 接入（无需真实 zlib），亦供 shell 自检做压缩回环。
 *
 * 重要：此处定义的 reduced-feature 宏必须与 Makefile 中构建 libminiz.a 的
 * MINIZ_CFLAGS 完全一致（MINIZ_NO_STDIO / MINIZ_NO_TIME / MINIZ_NO_ARCHIVE_APIS），
 * 否则头文件声明的 API 子集与已编译库不一致（本 libc 无 FILE* 流抽象，故必须
 * 关掉 stdio/archive API；无本地时区，故关 TIME）。
 */
#ifndef _SUKI_SHIM_ZLIB_H
#define _SUKI_SHIM_ZLIB_H

/* 与 build/libminiz.a 编译宏保持一致（单一真相源，避免 ABI 不一致）。 */
#define MINIZ_USE_ZLIB_COMPAT
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME

#include <kernel/abilities/miniz/miniz.h>

/* ---- 补齐 miniz 未提供、而 libcurl 等 zlib 消费者使用的兼容符号 ---- */

/* 真 zlib 在 ZLIB_CONST 下定义 z_const（curl 以 `z_const Bytef *` 声明输入缓冲）。 */
#ifndef z_const
#define z_const const
#endif

/* z_streamp 兼容别名（miniz 仅提供 mz_streamp）。 */
#ifndef z_streamp
#define z_streamp mz_streamp
#endif

/* miniz 无 inflateReset2()；以「结束 + 按新 windowBits 重新初始化」等价实现。
 * curl 在 deflate_do_* 路径中用它切换 raw/带包头的 window 语义。 */
#ifndef SUKI_MINIZ_INFLATE_RESET2_DEFINED
#define SUKI_MINIZ_INFLATE_RESET2_DEFINED
static inline int inflateReset2(z_streamp strm, int windowBits)
{
    (void)inflateEnd(strm);
    return inflateInit2(strm, windowBits);
}
#endif

#endif /* _SUKI_SHIM_ZLIB_H */
