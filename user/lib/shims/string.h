/*
 * user/lib/shims/string.h
 * -----------------------------------------------------------------------------
 * SukiOS 用户态标准 <string.h> 实现头（freestanding libc 提供）。
 *
 * 适用对象：
 *   - 第三方代码（如 drivers/FatFs/ff.c 的 strchr/strlen 调用）直接
 *     #include <string.h> 即可，无需感知 SukiOS 内部头。
 *   - SukiOS 自有代码经 libc.h 间接包含本文件，得到与 user/lib/string.c 及
 *     user/lib/suki.c 实现一一对应的函数原型。
 *
 * 实现归属：
 *   * mem* 系列        -> user/lib/suki.c
 *   * str* 系列        -> user/lib/string.c（strchr 在 suki.c）
 *   * strerror/strsignal -> user/lib/string.c
 * 本文件只做声明，不引入任何非标准依赖（仅依赖编译器自带的 <stddef.h>）。
 */
#ifndef _SUKI_SHIM_STRING_H
#define _SUKI_SHIM_STRING_H

#include <stddef.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ---- 内存操作（user/lib/suki.c） ---- */
void  *memcpy(void *dest, const void *src, size_t n);
void  *memmove(void *dest, const void *src, size_t n);
void  *memset(void *s, int c, size_t n);
int    memcmp(const void *s1, const void *s2, size_t n);
void  *memchr(const void *s, int c, size_t n);

/* ---- 字符串长度 / 比较 / 拷贝 / 拼接（user/lib/string.c） ---- */
size_t strlen(const char *s);
int    strcmp(const char *s1, const char *s2);
int    strncmp(const char *s1, const char *s2, size_t n);
char  *strcpy(char *dest, const char *src);
char  *strncpy(char *dest, const char *src, size_t n);
size_t strlcpy(char *dest, const char *src, size_t size);
char  *strcat(char *dest, const char *src);
char  *strncat(char *dest, const char *src, size_t n);
char  *strdup(const char *s);
char  *strndup(const char *s, size_t n);

/* ---- 查找 / 分词 ---- */
char  *strchr(const char *s, int c);          /* user/lib/suki.c */
char  *strrchr(const char *s, int c);         /* user/lib/string.c */
char  *strstr(const char *haystack, const char *needle);
char  *strtok(char *s, const char *delim);
char  *strtok_r(char *s, const char *delim, char **save);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char  *strpbrk(const char *s, const char *accept);

/* ---- 数值转换 ---- */
long   strtol(const char *nptr, char **endptr, int base);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
int    atoi(const char *nptr);
long   atol(const char *nptr);

/* ---- 错误/信号字符串（user/lib/string.c） ---- */
const char *strerror(int errnum);
const char *strsignal(int signum);

#endif /* _SUKI_SHIM_STRING_H */
