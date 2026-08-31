/*
 * user/lib/shims/stdio.h
 * -----------------------------------------------------------------------------
 * SukiOS 用户态标准 <stdio.h> 实现头（freestanding libc 提供，最小子集）。
 *
 * 实现归属：user/lib/stdio.c。本 libc 不提供 FILE* 流抽象（用户态直接面向
 * 内核 fd + 格式化输出），故仅提供无缓冲字符/字符串接口与格式化输出，不提供
 * fopen/fread 等文件流 API（可用 unistd.h 的 open/read/write 替代）。
 */
#ifndef _SUKI_SHIM_STDIO_H
#define _SUKI_SHIM_STDIO_H

#include <stddef.h>
#include <stdarg.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

/* 标准流文件号（SukiOS 复用 POSIX 文件号；无 FILE* 结构） */
#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

/* ---- 格式化输出（user/lib/stdio.c） ---- */
int printf(const char *fmt, ...);
int fprintf(int fd, const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

/* ---- 字符输出 ---- */
int putchar(int c);
int puts(const char *s);

#endif /* _SUKI_SHIM_STDIO_H */
