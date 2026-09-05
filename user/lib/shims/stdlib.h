/*
 * user/lib/shims/stdlib.h
 * -----------------------------------------------------------------------------
 * SukiOS 用户态标准 <stdlib.h> 实现头（freestanding libc 提供）。
 *
 * 实现归属：user/lib/stdlib.c（堆、环境变量、退出、字符串转数值）。
 * 注意：SukiOS 不提供 system()/tmpfile() 等需要宿主 OS 服务的接口；
 * 以下仅声明本 libc 实际实现的符号，未实现的 POSIX 函数一律不声明，避免误用。
 */
#ifndef _SUKI_SHIM_STDLIB_H
#define _SUKI_SHIM_STDLIB_H

#include <stddef.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

/* 退出码 */
#define EXIT_FAILURE 1
#define EXIT_SUCCESS 0
#define RAND_MAX    2147483647

/* ---- 内存分配（user/lib/stdlib.c + 简易堆后端） ---- */
void  *malloc(size_t size);
void  *calloc(size_t nmemb, size_t size);
void  *realloc(void *ptr, size_t size);
void   free(void *ptr);

/* ---- 进程 / 退出控制 ---- */
void   exit(int status);
void   abort(void);
int    atexit(void (*func)(void));

/* ---- 环境变量（进程内维护，user/lib/stdlib.c） ---- */
char  *getenv(const char *name);
int    setenv(const char *name, const char *value, int overwrite);
int    unsetenv(const char *name);
int    putenv(char *string);
extern char **environ;

/* ---- 字符串转数值 ---- */
long   strtol(const char *nptr, char **endptr, int base);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
int    atoi(const char *nptr);
long   atol(const char *nptr);

/* ---- 排序（FreeType 等需要；实现见 stdlib.c） ---- */
void   qsort(void *base, size_t nmemb, size_t size,
             int (*compar)(const void *, const void *));

#endif /* _SUKI_SHIM_STDLIB_H */
