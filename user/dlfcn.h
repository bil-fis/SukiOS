/*
 * user/dlfcn.h
 * 动态链接运行时接口（POSIX dlfcn）声明。
 * 注意：SukiOS 当前仅支持「立即绑定」语义（等同 RTLD_NOW），不支持 PLT 惰性绑定。
 */
#ifndef _SUKI_DLFCN_H
#define _SUKI_DLFCN_H

#include <stdint.h>
#include <stddef.h>

/* 标志（当前仅 RTLD_NOW 语义被实现，其余被忽略） */
#define RTLD_LAZY    0x1   /* 预留：惰性绑定（SukiOS 实际仍立即绑定） */
#define RTLD_NOW     0x2   /* 立即解析所有符号 */
#define RTLD_GLOBAL  0x4   /* 符号对外可见 */
#define RTLD_LOCAL   0x0   /* 符号仅本库可见 */

void  *dlopen(const char *path, int flags);
void  *dlsym(void *handle, const char *name);
int    dlclose(void *handle);
char  *dlerror(void);

#endif /* _SUKI_DLFCN_H */
