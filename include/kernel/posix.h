/*
 * include/kernel/posix.h
 * -----------------------------------------------------------------------------
 * 内核侧 POSIX 系统调用层的对外接口（实现见 kernel/syscall/sys_posix.c）。
 *
 * 与 <sukios/posix.h> 的分工：
 *   <sukios/posix.h>  = ABI 契约（号表 + 结构体 + 常量），内核与用户态共用；
 *   本文件            = 内核内部接口（初始化 + 分发入口），仅内核可见。
 *
 * 调用关系：
 *   kmain                       -> posix_init()
 *   syscall.c::syscall_dispatch -> posix_dispatch()（处理号区 0..SYSCALL_MAX-1）
 */
#ifndef _SUKI_KERNEL_POSIX_H
#define _SUKI_KERNEL_POSIX_H

#include <kernel/types.h>

/* 初始化 POSIX 子系统（fd 槽池等）。必须在 syscall_init() 之后、
 * 第一个用户任务创建之前调用。 */
void posix_init(void);

/*
 * POSIX 号区分发。
 *   a1..a6 对应用户态 rdi/rsi/rdx/r10/r8/r9（第 4 参走 r10，因 syscall
 *   指令占用 rcx 保存返回 RIP）；a6 由 syscall_entry.S 经栈传入。
 * 返回 true 表示 num 已被本层处理（*ret 为返回值：成功 >=0，失败 -errno）；
 * 返回 false 表示 num 不属于 POSIX 号区，调用方应按「未知系统调用」处理。
 */
bool posix_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                    uint64_t a4, uint64_t a5, uint64_t a6, int64_t *ret);

#endif /* _SUKI_KERNEL_POSIX_H */
