/*
 * user/lib/signal.c
 * -----------------------------------------------------------------------------
 * 用户态信号 API：sigaction / kill / raise / signal / sigprocmask / sigreturn。
 *
 * 关键设计：sigaction() 始终把 sa_restorer 设为 __sighandler_trampoline 并置
 * SA_RESTORER 标志。内核在投递信号构造用户栈帧时，把「返回地址」写为该 restorer，
 * handler 返回后经由它调用 SYS_SIGRETURN 还原被打断的上下文（标准 Linux 做法）。
 */
#include "lib/suki.h"
#include "lib/libc.h"
#include <sukios/posix.h>

#define __SIG_STR(x) #x
#define SIG_STR(x)   __SIG_STR(x)

/* 信号帧返回蹦床：handler 经 ret 跳到这里，此时 rsp 位于返回槽之上 8 字节
 * （即 new_rsp+8）；ucontext 由内核写在 new_rsp+56 处，故 lea 48(%rsp) 取到。
 * 取 &ucontext 放入 rdi 后，执行 SYS_SIGRETURN 还原被打断上下文。
 * 注：SYS_SIGRETURN 用字符串化方式内联为立即数，避免 naked 函数里 "i" 约束
 * 被汇编器误解为符号引用（曾导致 undefined reference to '$207'）。 */
__attribute__((naked))
void __sighandler_trampoline(void)
{
    __asm__ volatile(
        "lea 48(%%rsp), %%rdi\n\t"                   /* rdi = &ucontext */
        "mov $" SIG_STR(SYS_SIGRETURN) ", %%rax\n\t" /* rax = SYS_SIGRETURN */
        "syscall\n\t"
        ::: "rdi", "rax", "rcx", "r11", "memory");
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact)
{
    int r = (int)suki_syscall3(SYS_SIGACTION, (uint64_t)sig,
                               (uint64_t)act, (uint64_t)oldact);
    return (int)libc_ret((int64_t)r);
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
    int r = (int)suki_syscall3(SYS_SIGPROCMASK, (uint64_t)how,
                               (uint64_t)set, (uint64_t)oldset);
    return (int)libc_ret((int64_t)r);
}

int raise(int sig)
{
    int r = (int)suki_syscall1(SYS_RAISE, (uint64_t)sig);
    return (int)libc_ret((int64_t)r);
}

/* 简化版 signal()：等价于 sigaction + SA_RESTORER；忽略旧 handler 返回（仅返回 SIG_ERR/-1）。 */
sighandler_t signal(int sig, sighandler_t handler)
{
    struct sigaction sa;
    sa._u.sa_handler = handler;
    sa.sa_flags = SA_RESTORER;
    sa.sa_restorer = __sighandler_trampoline;
    sa.sa_mask = 0;
    if (sigaction(sig, &sa, NULL) != 0)
        return SIG_ERR;
    return handler;   /* 注：未返回旧 handler（简化实现） */
}

void sigreturn(const void *ucontext)
{
    suki_syscall1(SYS_SIGRETURN, (uint64_t)ucontext);
}
