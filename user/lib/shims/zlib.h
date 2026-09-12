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

#endif /* _SUKI_SHIM_ZLIB_H */
