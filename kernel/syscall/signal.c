/*
 * kernel/syscall/signal.c
 * -----------------------------------------------------------------------------
 * 信号（signal/sigaction）内核实现：
 *   - 用户态 handler 投递：在 syscall 返回路径（syscall_return_path）经
 *     sig_deliver_check() 注入，构造用户态信号帧（ucontext + siginfo + 返回地址），
 *     改写 iret 帧跳转 handler；handler 返回后经 trampoline -> SYS_SIGRETURN 还原。
 *   - 默认动作：SIG_DFL 多数信号终止任务（128+sig），部分信号（SIGCHLD/SIGURG/
 *     SIGCONT/SIGWINCH）默认忽略；SIGKILL/SIGSTOP 不可捕获/阻塞。
 *   - kill/tkill/raise 仅设置 pending 位；真实投递发生在目标任务此后任意一次返回
 *     用户态时（syscall_return_path）。
 *
 * 用户段常量（与内核约定一致，见 task.h / posix.h）。
 */
#include <kernel/task.h>
#include <kernel/syscall.h>
#include <kernel/posix.h>
#include <kernel/spinlock.h>
#include <string.h>

extern uint64_t g_syscall_gpr[MAX_CPUS];   /* 本核最近一次 syscall 的用户 GPR 帧基址 */
extern uint64_t *g_scratch[MAX_CPUS];        /* 本核 task 的 scr_rip/scr_rsp 指针 */

/* 用户段选择子 / 标志（与 sys_posix.c 中 fork 帧约定一致） */
#define SIG_USER_CS  0x1B
#define SIG_USER_DS  0x23
#define SIG_USER_RFL 0x202

/* 内核帧布局（syscall_return_path 中 rsp 指向 GPR 槽底）：
 *   GPR 槽 [0..11]：r9,r8,r10,rdx,rsi,rdi,r15,r14,r13,r12,rbp,rbx
 *   iret 帧 [12..16]：RIP, CS, RFLAGS, RSP, SS
 * 严格对照 kernel/arch/x86_64/syscall_entry.S 的 push 顺序。 */
#define FR_R9    (0 * 8)
#define FR_R8    (1 * 8)
#define FR_R10   (2 * 8)
#define FR_RDX   (3 * 8)   /* rdx 位于第 4 个 GPR 槽（r9,r8,r10,rdx...） */
#define FR_RSI   (4 * 8)
#define FR_RDI   (5 * 8)
#define FR_R15   (6 * 8)
#define FR_R14   (7 * 8)
#define FR_R13   (8 * 8)
#define FR_R12   (9 * 8)
#define FR_RBP   (10 * 8)
#define FR_RBX   (11 * 8)
#define FR_IP    (12 * 8)
#define FR_CS    (13 * 8)
#define FR_FL    (14 * 8)
#define FR_SP    (15 * 8)
#define FR_SS    (16 * 8)

/* 默认忽略的信号（POSIX 语义） */
static bool sig_default_ignore(int sig)
{
    return sig == SUKI_SIGCHLD || sig == SUKI_SIGURG || sig == SUKI_SIGCONT ||
           sig == SUKI_SIGWINCH;
}

/* 向任务 t 投递信号 sig：置 pending 位并（若阻塞于内核）唤醒以尽快返回用户态检查。 */
void task_signal_send(task_t *t, int sig)
{
    if (!t || sig <= 0 || sig >= SUKI_NSIG)
        return;
    t->pending |= (1ull << (sig - 1));
    /* 若目标正阻塞等待（非 RUNNING），唤醒它，使其返回用户态时检查 pending 信号。 */
    if (t->state != RUNNING)
        sched_wake(t);
}

/* ---- 默认动作：终止当前任务（SIG_DFL 终止类信号） ---- */
static void signal_terminate(int sig)
{
    /* 不走正常 syscall 返回，直接以 (128+sig) 退出。task_exit_current 不返回。 */
    task_exit_current((uint64_t)128 + (uint64_t)sig);
}

/* 在 syscall 返回路径（posix_dispatch 返回前）被调用：投递一个待处理且未阻塞的信号。
 * rax = 当前 syscall 返回值（无投递时原样返回）；frame = 内核帧基址。
 *
 * 关键：syscall_return_path 末尾会用 g_scratch[cpu]->scr_rip/scr_rsp 覆盖 iret 帧的
 * RIP/RSP，因此本函数必须同步更新 g_scratch（scr[0]=scr_rip, scr[1]=scr_rsp），否则
 * 只改 f[FR_IP] 会被覆盖回原返回地址，handler 永远跑不起来（曾导致 iret #GP CRASH）。 */
uint64_t sig_deliver_check(uint64_t rax, uint64_t frame)
{
    task_t *cur = sched_current();
    if (!cur || !cur->is_user)
        return rax;

    /* 已在 handler 中：不重入投递，待 handler 经 sigreturn 返回后再检查。 */
    if (cur->in_signal)
        return rax;

    /* 找到第一个 pending 且未阻塞的信号 */
    int sig = -1;
    for (int s = 1; s < SUKI_NSIG; s++) {
        if ((cur->pending & (1ull << (s - 1))) &&
            !(cur->sigmask & (1ull << (s - 1)))) {
            sig = s;
            break;
        }
    }
    if (sig < 0)
        return rax;   /* 无待投递信号：rax 原样返回 */

    /* 取得处置 */
    struct suki_sigaction *sa = &cur->sigactions[sig];
    void (*hdl)(int) = sa->_u.sa_handler;

    /* SIGKILL / SIGSTOP 不可捕获：强制默认动作 */
    if (sig == SUKI_SIGKILL) {
        cur->pending &= ~(1ull << (sig - 1));
        signal_terminate(sig);
        /* 不返回 */
    }
    if (sig == SUKI_SIGSTOP) {
        cur->pending &= ~(1ull << (sig - 1));
        return rax;   /* SIGSTOP 默认忽略（不实现暂停态） */
    }

    if (hdl == SUKI_SIG_IGN) {
        cur->pending &= ~(1ull << (sig - 1));
        return rax;   /* 忽略 */
    }
    if (hdl == SUKI_SIG_DFL) {
        cur->pending &= ~(1ull << (sig - 1));
        if (sig_default_ignore(sig))
            return rax;
        signal_terminate(sig);
        /* 不返回 */
    }

    /* 真实 handler：构造用户态信号帧 */
    uint64_t *f = (uint64_t *)frame;

    struct suki_ucontext uc;
    memset(&uc, 0, sizeof(uc));
    for (int i = 0; i < 12; i++)
        uc.gpr[i] = f[i];
    uc.rax    = rax;
    uc.rip    = f[FR_IP / 8];
    uc.rflags = f[FR_FL / 8];
    uc.rsp    = f[FR_SP / 8];
    uc.cr2    = 0;
    uc.sigmask = cur->sigmask;
    fpu_fxsave(&uc.fpu);

    struct suki_siginfo info;
    memset(&info, 0, sizeof(info));
    info.si_signo = sig;
    info.si_code  = SUKI_SI_TKILL;
    info.si_pid   = (int64_t)cur->tid;
    info.si_uid   = (int64_t)cur->uid;

    /* 用户栈上新帧：[trampoline:8][siginfo][ucontext]，按 16 字节对齐向下生长 */
    size_t framesz = sizeof(uint64_t) + sizeof(struct suki_siginfo) +
                     sizeof(struct suki_ucontext);
    framesz = (framesz + 15) & ~(size_t)15;
    uint64_t new_rsp = (f[FR_SP / 8] - framesz) & ~(uint64_t)15;

    void (*restorer)(void) = sa->sa_restorer;
    if (!restorer)
        return rax;   /* 无 restorer（libc 未设置）：安全跳过，不破坏上下文 */

    /* 写回用户栈 */
    if (copy_to_user((void *)new_rsp, &restorer, 8) != 8)
        return rax;
    if (copy_to_user((void *)(new_rsp + 8), &info, sizeof(info)) != sizeof(info))
        return rax;
    if (copy_to_user((void *)(new_rsp + 8 + sizeof(struct suki_siginfo)),
                     &uc, sizeof(uc)) != sizeof(uc))
        return rax;

    /* 改写内核帧：GPR 参数 + iret 跳 handler */
    f[FR_RDI / 8] = (uint64_t)sig;                          /* rdi = sig */
    f[FR_RSI / 8] = new_rsp + 8;                            /* rsi = &siginfo */
    f[FR_RDX / 8] = new_rsp + 8 + sizeof(struct suki_siginfo); /* rdx = &ucontext */
    f[FR_IP / 8] = (uint64_t)hdl;                           /* RIP = handler */
    f[FR_CS / 8] = SIG_USER_CS;
    f[FR_FL / 8] = SIG_USER_RFL;
    f[FR_SP / 8] = new_rsp;                                /* RSP = 新用户栈顶 */
    f[FR_SS / 8] = SIG_USER_DS;

    /* 关键：同步更新 g_scratch，syscall_return_path 会用它覆盖 iret 帧 RIP/RSP。 */
    uint64_t *scr = g_scratch[cpu_index()];                /* scr[0]=scr_rip, scr[1]=scr_rsp */
    scr[0] = (uint64_t)hdl;
    scr[1] = new_rsp;

    /* 更新任务信号状态 */
    cur->pending    &= ~(1ull << (sig - 1));
    cur->saved_sigmask = cur->sigmask;
    cur->sigmask |= (1ull << (sig - 1));                   /* 阻塞本信号 */
    cur->sigmask |= sa->sa_mask;                           /* + sa_mask */
    cur->in_signal++;

    return rax;   /* handler 入口不依赖 rax */
}

/* ===================== 系统调用 ===================== */

/* sigaction(sig, act, oldact) */
int64_t sys_sigaction(uint64_t a1, uint64_t a2, uint64_t a3)
{
    int sig = (int)a1;
    const struct suki_sigaction *act = (const struct suki_sigaction *)a2;
    struct suki_sigaction *oldact = (struct suki_sigaction *)a3;
    task_t *cur = sched_current();

    if (sig <= 0 || sig >= SUKI_NSIG)
        return -SUKI_EINVAL;
    if (sig == SUKI_SIGKILL || sig == SUKI_SIGSTOP)
        return -SUKI_EINVAL;   /* 不可捕获/忽略 */

    if (oldact) {
        if (copy_to_user(oldact, &cur->sigactions[sig],
                         sizeof(struct suki_sigaction)) != sizeof(struct suki_sigaction))
            return -SUKI_EFAULT;
    }
    if (act) {
        struct suki_sigaction tmp;
        if (copy_from_user(&tmp, act, sizeof(tmp)) != sizeof(tmp))
            return -SUKI_EFAULT;
        cur->sigactions[sig] = tmp;
        /* 安装新 handler 时清掉可能残留的 pending（避免陈旧投递） */
        cur->pending &= ~(1ull << (sig - 1));
    }
    return 0;
}

/* sigreturn(ucontext*)：还原被信号打断的上下文 */
int64_t sys_sigreturn(uint64_t a1)
{
    task_t *cur = sched_current();
    const struct suki_ucontext *uctx = (const struct suki_ucontext *)a1;
    struct suki_ucontext uc;
    if (copy_from_user(&uc, uctx, sizeof(uc)) != sizeof(uc))
        return -SUKI_EFAULT;

    uint64_t frame = g_syscall_gpr[cpu_index()];
    uint64_t *f = (uint64_t *)frame;
    for (int i = 0; i < 12; i++)
        f[i] = uc.gpr[i];
    f[FR_IP / 8] = uc.rip;
    f[FR_CS / 8] = SIG_USER_CS;
    f[FR_FL / 8] = uc.rflags;
    f[FR_SP / 8] = uc.rsp;
    f[FR_SS / 8] = SIG_USER_DS;
    fpu_fxrstor(&uc.fpu);

    /* 关键：sys_sigreturn 在 syscall_dispatch 内部执行，早于 syscall_return_path
     * 中「用 g_scratch->scr_rip/scr_rsp 覆盖 iret 帧 RIP/RSP」的代码，因此仅改
     * 帧 f[FR_IP/FR_SP] 会被覆盖回 trampoline 地址。必须同步更新 g_scratch 指向的
     * 当前任务 scr_rip/scr_rsp，返回路径覆盖后才是真正的原始上下文。 */
    uint64_t *scr = g_scratch[cpu_index()];   /* scr[0]=scr_rip, scr[1]=scr_rsp */
    scr[0] = uc.rip;
    scr[1] = uc.rsp;

    cur->sigmask = uc.sigmask;
    if (cur->in_signal)
        cur->in_signal--;

    return (int64_t)uc.rax;   /* 用户 rax 由 syscall 返回路径透传 */
}

/* sigprocmask(how, set, oldset) */
int64_t sys_sigprocmask(uint64_t a1, uint64_t a2, uint64_t a3)
{
    int how = (int)a1;
    const suki_sigset_t *set = (const suki_sigset_t *)a2;
    suki_sigset_t *oldset = (suki_sigset_t *)a3;
    task_t *cur = sched_current();

    suki_sigset_t old = cur->sigmask;
    if (oldset) {
        if (copy_to_user(oldset, &old, sizeof(old)) != sizeof(old))
            return -SUKI_EFAULT;
    }
    if (set) {
        suki_sigset_t s;
        if (copy_from_user(&s, set, sizeof(s)) != sizeof(s))
            return -SUKI_EFAULT;
        switch (how) {
        case SUKI_SIG_BLOCK:   cur->sigmask |= s; break;
        case SUKI_SIG_UNBLOCK: cur->sigmask &= ~s; break;
        case SUKI_SIG_SETMASK: cur->sigmask = s; break;
        default: return -SUKI_EINVAL;
        }
        /* SIGKILL/SIGSTOP 永远不可阻塞 */
        cur->sigmask &= ~((1ull << (SUKI_SIGKILL - 1)) |
                          (1ull << (SUKI_SIGSTOP - 1)));
    }
    return 0;
}

/* tkill(tid, sig)：向指定线程（tid==task id）发送信号 */
int64_t sys_tkill(uint64_t a1, uint64_t a2)
{
    int tid = (int)a1;
    int sig = (int)a2;
    if (sig <= 0 || sig >= SUKI_NSIG)
        return -SUKI_EINVAL;
    task_t *t = task_lookup((uint64_t)tid);
    if (!t)
        return -SUKI_ESRCH;
    task_signal_send(t, sig);
    return 0;
}

/* raise(sig)：向自身（当前线程）发送 */
int64_t sys_raise(uint64_t a1)
{
    int sig = (int)a1;
    task_signal_send(sched_current(), sig);
    return 0;
}

/* 注：kill 的号区分发由 sys_posix.c 中的 sys_kill 处理；本文件仅提供
 * task_signal_send / sys_tkill / sys_raise 等信号投递原语。 */
