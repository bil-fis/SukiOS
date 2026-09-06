/*
 * user/lib/dlfcn.c
 * libdl：动态链接运行时接口封装（dlopen/dlsym/dlclose/dlerror）。
 *
 * 实际的「加载共享库 + 符号解析 + 重定位」完全由内核完成
 * （系统调用 126..129 → elf.c 的 elf_dlopen/elf_dlsym/elf_dlclose）。
 * 本文件只做系统调用封装与错误字符串缓存。
 */
#include "libc.h"            /* 提供 suki_syscallN 宏 */
#include <sukios/posix.h>    /* SYS_DL_OPEN / SYS_DL_SYM / SYS_DL_CLOSE / SYS_DL_ERROR */
#include <dlfcn.h>
#include <stdint.h>

static char g_dl_err[64];

void *dlopen(const char *path, int flags)
{
    (void)flags;   /* SukiOS 当前仅立即绑定语义，忽略 flags */
    long h = suki_syscall1(SYS_DL_OPEN, (uint64_t)(uintptr_t)path);
    if (h <= 0) {
        g_dl_err[0] = '\0';
        return NULL;
    }
    return (void *)(uintptr_t)h;
}

void *dlsym(void *handle, const char *name)
{
    long addr = suki_syscall2(SYS_DL_SYM,
                              (uint64_t)(uintptr_t)handle,
                              (uint64_t)(uintptr_t)name);
    if (addr == 0) {
        return NULL;
    }
    return (void *)(uintptr_t)addr;
}

int dlclose(void *handle)
{
    return (int)suki_syscall1(SYS_DL_CLOSE, (uint64_t)(uintptr_t)handle);
}

char *dlerror(void)
{
    suki_syscall0(SYS_DL_ERROR);
    return (g_dl_err[0] != '\0') ? g_dl_err : NULL;
}
