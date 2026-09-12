/*
 * user/lib/shims/stdio.h
 * -----------------------------------------------------------------------------
 * SukiOS 用户态标准 <stdio.h>（freestanding libc 提供）。
 *
 * 实现归属：
 *   - 格式化/无缓冲输出：user/lib/stdio.c（printf/snprintf/puts/putchar）
 *   - FILE* 流抽象：user/lib/fileio.c（fopen/fread/fwrite/fgets/fprintf/std*）
 *
 * FILE 为内核 fd 的薄封装（结构体在 fileio.c 中定义，此处仅前置声明），
 * 提供第三方库（如 libcurl）所需的 stdin/stdout/stderr 与 fread/fwrite 等符号。
 */
#ifndef _SUKI_SHIM_STDIO_H
#define _SUKI_SHIM_STDIO_H

#include <stddef.h>
#include <stdarg.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

/* 标准流文件号（SukiOS 复用 POSIX 文件号） */
#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

#ifndef EOF
#define EOF (-1)
#endif

#ifndef SEEK_SET
#define SEEK_SET 0
#endif
#ifndef SEEK_CUR
#define SEEK_CUR 1
#endif
#ifndef SEEK_END
#define SEEK_END 2
#endif

#ifndef BUFSIZ
#define BUFSIZ 512
#endif

/* ---- FILE* 流（user/lib/fileio.c） ---- */
typedef struct _SUKI_FILE FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE  *fopen(const char *path, const char *mode);
FILE  *fdopen(int fd, const char *mode);
FILE  *freopen(const char *path, const char *mode, FILE *stream);
int    fclose(FILE *fp);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp);
int    fflush(FILE *fp);
int    ferror(FILE *fp);
int    feof(FILE *fp);
void   clearerr(FILE *fp);
char  *fgets(char *s, int size, FILE *fp);
int    fputc(int c, FILE *fp);
int    fgetc(FILE *fp);
int    fputs(const char *s, FILE *fp);
int    fprintf(FILE *fp, const char *fmt, ...);
int    vfprintf(FILE *fp, const char *fmt, va_list ap);
int    fileno(FILE *fp);
int    fseek(FILE *fp, long offset, int whence);
long   ftell(FILE *fp);
void   rewind(FILE *fp);

/* ---- 格式化输出（user/lib/stdio.c） ---- */
int printf(const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

/* ---- 字符输出 ---- */
int putchar(int c);
int puts(const char *s);

#endif /* _SUKI_SHIM_STDIO_H */
