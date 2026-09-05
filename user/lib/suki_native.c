/*
 * user/lib/suki_native.c
 * -----------------------------------------------------------------------------
 * SukiNative 原生对象 API 的用户态封装（libsuki）。对应内核
 * kernel/syscall/sys_suki.c 的 130..149 号系统调用。
 *
 * 与 POSIX 平行：新服务可直接调用这些原生接口（一切皆对象，返回 suki_handle_t
 * 句柄，用 suki_status_t 状态码 0/负 errno）。POSIX 兼容层（20..129）不受影响。
 *
 * __suki_syscallN 的声明来自 <sukios/posix.h>（static inline 内联汇编，ABI：
 * rax=号，rdi/rsi/rdx/r10/r8/r9=参数）。
 */
#include "suki.h"
#include <sukios/posix.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

suki_status_t suki_event_create(bool init_signaled, bool manual_reset, suki_handle_t *out)
{
    int64_t h = __suki_syscall3(SYS_SUKI_EVENT_CREATE,
                                (uint64_t)init_signaled, (uint64_t)manual_reset, 0);
    if (h < 0)
        return (suki_status_t)h;
    *out = (suki_handle_t)h;
    return 0;
}

suki_status_t suki_event_set(suki_handle_t h)
{
    return (suki_status_t)__suki_syscall1(SYS_SUKI_EVENT_SET, (uint64_t)h);
}

suki_status_t suki_event_reset(suki_handle_t h)
{
    return (suki_status_t)__suki_syscall1(SYS_SUKI_EVENT_RESET, (uint64_t)h);
}

suki_status_t suki_mutex_create(suki_handle_t *out)
{
    int64_t h = __suki_syscall1(SYS_SUKI_MUTEX_CREATE, 0);
    if (h < 0)
        return (suki_status_t)h;
    *out = (suki_handle_t)h;
    return 0;
}

suki_status_t suki_mutex_lock(suki_handle_t h)
{
    return (suki_status_t)__suki_syscall1(SYS_SUKI_MUTEX_LOCK, (uint64_t)h);
}

suki_status_t suki_mutex_unlock(suki_handle_t h)
{
    return (suki_status_t)__suki_syscall1(SYS_SUKI_MUTEX_UNLOCK, (uint64_t)h);
}

suki_status_t suki_sem_create(uint32_t initial, uint32_t max, suki_handle_t *out)
{
    int64_t h = __suki_syscall3(SYS_SUKI_SEM_CREATE,
                                (uint64_t)initial, (uint64_t)max, 0);
    if (h < 0)
        return (suki_status_t)h;
    *out = (suki_handle_t)h;
    return 0;
}

suki_status_t suki_sem_acquire(suki_handle_t h)
{
    return (suki_status_t)__suki_syscall1(SYS_SUKI_SEM_ACQUIRE, (uint64_t)h);
}

suki_status_t suki_sem_release(suki_handle_t h)
{
    return (suki_status_t)__suki_syscall1(SYS_SUKI_SEM_RELEASE, (uint64_t)h);
}

suki_status_t suki_wait(const suki_handle_t *handles, size_t count, uint32_t flags,
                       uint64_t timeout_ms, size_t *out_index)
{
    (void)timeout_ms;   /* 暂未支持超时：阻塞至被唤醒 */
    size_t idx = 0;
    int64_t r = __suki_syscall6(SYS_SUKI_WAIT,
                                (uint64_t)handles, (uint64_t)count, (uint64_t)flags,
                                0, (uint64_t)&idx, 0);
    if (r == 0)
        *out_index = idx;
    return (suki_status_t)r;
}

suki_status_t suki_obj_destroy(suki_handle_t h)
{
    return (suki_status_t)__suki_syscall1(SYS_SUKI_OBJ_DESTROY, (uint64_t)h);
}

suki_status_t suki_obj_duplicate(suki_handle_t h, uint32_t rights, suki_handle_t *out)
{
    int64_t nh = __suki_syscall2(SYS_SUKI_OBJ_DUPLICATE, (uint64_t)h, (uint64_t)rights);
    if (nh < 0)
        return (suki_status_t)nh;
    *out = (suki_handle_t)nh;
    return 0;
}

suki_status_t suki_obj_query(suki_handle_t h, suki_objinfo_t *info)
{
    return (suki_status_t)__suki_syscall2(SYS_SUKI_OBJ_QUERY,
                                          (uint64_t)h, (uint64_t)info);
}

suki_status_t suki_obj_create(uint32_t type, uint64_t a2, uint64_t a3, suki_handle_t *out)
{
    int64_t h = __suki_syscall3(SYS_SUKI_OBJ_CREATE, (uint64_t)type, a2, a3);
    if (h < 0)
        return (suki_status_t)h;
    *out = (suki_handle_t)h;
    return 0;
}
