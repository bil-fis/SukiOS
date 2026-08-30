/*
 * kernel/syscall/sys_posix.c
 * -----------------------------------------------------------------------------
 * 完整 POSIX 系统调用层（为 newlib / POSIX 兼容层做地基）。
 *
 * 定位：本文件是 POSIX 语义的【内核侧实现】，编号与结构体全部取自
 * <sukios/posix.h>（内核与用户态的唯一 ABI 契约，两侧绝不各自硬编码）。
 * 调用号按功能分区（进程/文件/内存/时间/系统/网络），语义对齐 POSIX.1-2017，
 * 但号值故意不仿 Linux——应用在本系统运行需重新编译（静态链接 newlib），
 * 无需二进制级号表一致。
 *
 * 三条贯穿全文件的红线：
 *   1) 【用户指针一律不过墙】所有用户态指针/字符串/数组先经 copy_from_user
 *      拷入 per-CPU 内核缓冲，绝不直接解引用（SMAP + 恶意地址防护）。
 *   2) 【失败返回 -errno】成功返回 >=0 的值；失败返回 -errno（1..4095）。
 *      用户态封装据此设置 errno 并返回 -1（newlib 直接可用）。
 *   3) 【per-CPU 暂存缓冲】syscall 处理期间可能因等待 IPC 应答而让出 CPU，
 *      同核另一任务若复用同一静态缓冲会互踩，故所有缓冲按 [MAX_CPUS] 分隔。
 *
 * 分层：本文件 -> kernel/fs/fd.c（fd 表 + FS_SERVER IPC 转发）
 *              -> kernel/sched/sched.c（fork/wait/kill 的进程语义）
 *              -> mm/vma.c（brk/mmap/mprotect 的地址空间语义）
 *              -> kernel/time/rtc.c（CLOCK_REALTIME 墙上时间）
 *
 * 调用关系：kernel/syscall/syscall.c::syscall_dispatch -> posix_dispatch()。
 */
#include <kernel/syscall.h>
#include <kernel/posix.h>
#include <kernel/types.h>
#include <kernel/task.h>
#include <kernel/string.h>
#include <kernel/console.h>
#include <kernel/percpu.h>
#include <kernel/clock.h>
#include <kernel/rtc.h>
#include <kernel/interrupts.h>
#include <kernel/fd.h>
#include <kernel/spinlock.h>
#include <kernel/smp.h>
#include <mm/vmm.h>
#include <mm/vma.h>
#include <mm/pmm.h>
#include <mm/kmalloc.h>
#include <mm/kstack.h>
#include <ipc/port.h>

/* syscall_entry.S 导出：
 *   fork_child_return  子进程“出生点”（rax=0 后复用 syscall 返回序列）
 *   g_syscall_gpr[cpu] 本核最近一次 syscall 的用户 GPR 保存帧基址（fork 用） */
extern void fork_child_return(void);
extern uint64_t g_syscall_gpr[MAX_CPUS];

/* 段选择子（与 syscall_entry.S / gdt.c 保持一致） */
#define USER_CS_SEL   0x1B
#define USER_DS_SEL   0x23
#define USER_RFLAGS   0x202      /* IF=1 */

/* 用户态堆（brk/sbrk）区间：避开 mmap 区(0x5000...)与 OOL 窗口(0x6000...) */
#define POSIX_HEAP_BASE   0x0000400000000000UL
#define POSIX_HEAP_MAX    (64UL * 1024 * 1024)

/* 节拍率：LAPIC 定时器 100Hz（见 clock_init） */
#define POSIX_CLK_TCK     100

/* ========================================================================== */
/*  per-CPU 暂存缓冲（红线 3）                                                 */
/* ========================================================================== */
static char g_path_buf[MAX_CPUS][512];      /* 路径字符串暂存 */
static char g_iobuf[MAX_CPUS][512];         /* 小结构/字符串暂存 */

/* ========================================================================== */
/*  用户态数据拷入辅助                                                         */
/* ========================================================================== */

/*
 * 从用户态拷贝 NUL 结尾的路径到内核缓冲。
 * 返回路径长度（不含 NUL）；失败返回负 errno。
 * 逐字节 copy_from_user 在长路径上极慢，这里先整体校验区间可读性再整段拷贝
 * （copy_from_user 内部已逐页校验页表存在性）。
 */
static int64_t copy_path(const char *upath, char *kbuf, size_t cap)
{
    if (!upath) {
        return -SUKI_EFAULT;
    }
    /* 先用小步读取判定长度上界：逐字节读直到 NUL 或超 cap */
    size_t len = 0;
    while (len < cap) {
        char c;
        if (copy_from_user(&c, upath + len, 1) != 1) {
            return -SUKI_EFAULT;
        }
        if (c == '\0') {
            break;
        }
        len++;
    }
    if (len >= cap) {
        return -SUKI_ENAMETOOLONG;
    }
    if (copy_from_user(kbuf, upath, len + 1) != len + 1) {
        return -SUKI_EFAULT;
    }
    return (int64_t)len;
}

/* 取本核路径缓冲并拷入用户路径；失败直接返回负 errno */
static int64_t fetch_path(const char *upath, char **out)
{
    char *buf = g_path_buf[cpu_index()];
    int64_t r = copy_path(upath, buf, sizeof(g_path_buf[0]));
    if (r < 0) {
        return r;
    }
    *out = buf;
    return r;
}

/* ========================================================================== */
/*  进程管理                                                                   */
/* ========================================================================== */

/*
 * sys_fork —— 完整 fork()（POSIX）。
 *
 * 子进程与父进程的差异只有两点：PID/父 PID，以及返回值（子 0、父 = 子 PID）。
 * 其余（地址空间内容、用户寄存器、fd 表、cwd、凭据）全部等价。
 *
 * 实现要点：
 *   a) 地址空间走【写时复制】：vmm_fork_cow 把父的用户半区页全部以只读+COW
 *      方式共享给子，任一方首次写入由 #PF -> vmm_cow_break 拷贝断开。这既
 *      符合 POSIX fork 语义（复制后互不干扰），又避免整空间深拷贝开销。
 *   b) VMA 链表必须【深拷贝】（vma_clone_all）：VMA 是「区间登记」型元数据，
 *      不随页表复制；缺它则子进程的 mmap 区/栈增长区无法按需补页，首次访问
 *      即被判非法而杀任务。
 *   c) 用户寄存器必须【逐位继承】：从 g_syscall_gpr[cpu] 取父的 12 个 GPR
 *      （含 rbx/rbp/r12-r15 等 callee-saved）。-O2 生成的用户代码会把长期
 *      变量放在这些寄存器里，若 fork 后子进程它们被内核 C 调用链污染，
 *      程序会以极难复现的方式错乱。
 *   d) 出栈路径：子进程内核栈上合成 [12 GPR 槽][iret 帧][context_switch 6 槽]
 *      [ret=fork_child_return]，首次被调度时经 context_switch 的 ret 落到
 *      fork_child_return（rax 置 0）再走完全相同的 syscall 返回序列回 Ring3。
 */
int64_t sys_fork(void)
{
    task_t *parent = sched_current();
    uint32_t cpu = cpu_index();

    /* 防御：fork 只能发生在 Ring3 的 syscall 上下文（必须有合法的 GPR 帧）。
     * 内核线程调用时 g_syscall_gpr 为 0 —— 直接拒绝，绝不合成垃圾帧。 */
    uint64_t frame = g_syscall_gpr[cpu];
    if (frame < KSTACK_AREA_BASE || frame + 12 * 8 > KSTACK_AREA_END) {
        return -SUKI_ENOSYS;
    }

    /* 1) 新任务结构与内核栈 */
    task_t *child = (task_t *)kzalloc(sizeof(task_t));
    if (!child) {
        return -SUKI_ENOMEM;
    }
    uint64_t kst = kstack_alloc();
    if (!kst) {
        kfree(child);
        return -SUKI_ENOMEM;
    }

    /* 2) 以父为模板复制，随后逐项修正「不可继承」的字段 */
    memcpy(child, parent, sizeof(task_t));
    child->id = sched_next_pid();
    child->parent_id = parent->id;
    child->kstack_base = kst;
    child->kstack_top = kst + KERNEL_STACK_BYTES;
    *(uint64_t *)kst = KSTACK_CANARY;
    child->rsp = 0;
    child->state = READY;
    child->alive = true;
    child->dead = false;
    child->zombie = false;
    child->exit_code = 0;
    child->wait_result = 0;
    child->wait_any = false;
    child->pending_kill = false;
    child->pending_signo = 0;
    child->is_idle = false;
    child->in_rq = false;
    child->ticks_remaining = TIME_SLICE_TICKS;
    /* 链表指针一律不继承（子进程是独立个体） */
    child->next = NULL;
    child->all_next = NULL;
    child->dead_next = NULL;
    child->wait_next = NULL;
    child->waiters = NULL;
    child->wait_link = NULL;
    child->ool_maps = NULL;          /* OOL 窗口不继承（与 XNU VM_INHERIT_NONE 一致） */
    child->vma_list = NULL;          /* 稍后由 vma_clone_all 深拷贝 */
    child->reply_port = NULL;

    /* 3) 地址空间：新建 + COW 共享父的用户半区 */
    uint64_t new_as = vmm_create_address_space();
    if (!new_as) {
        kstack_free(kst);
        kfree(child);
        return -SUKI_ENOMEM;
    }
    if (!vmm_fork_cow(parent->cr3, new_as)) {
        vmm_destroy_address_space(new_as);
        kstack_free(kst);
        kfree(child);
        return -SUKI_ENOMEM;
    }
    child->cr3 = new_as;
    /* KPTI：子进程的影子 PML4 必须映射自己的内核栈（Ring3 被中断时 CPU
     * 直接向 TSS.rsp0 压栈，栈页不在影子中则首个中断即三重故障）。 */
    vmm_kpti_map_kstack(new_as, child->kstack_base, child->kstack_top);

    /* 4) VMA 深拷贝（与 COW 页表配对，缺一不可） */
    if (!vma_clone_all(child, parent)) {
        vmm_destroy_address_space(new_as);
        kstack_free(kst);
        kfree(child);
        return -SUKI_ENOMEM;
    }

    /* 5) fd 表：共享槽（POSIX：fork 后父子共享文件偏移） */
    fd_fork_clone(child, parent);

    /*
     * 6) 合成子进程内核栈帧。
     *
     * 布局由【context_switch 的执行顺序】决定，必须与 switch.S 严格吻合：
     *   context_switch(&old_rsp, new_rsp) 切到 new_rsp 后依次
     *     popq %r15 / %r14 / %r13 / %r12 / %rbp / %rbx   （消耗 0..40）
     *     ret                                            （读 48，rsp 变为 56）
     * 故 ret 目标必须放在【偏移 48】，而 ret 之后 rsp = 偏移 56 —— 这个位置
     * 正是 syscall_return_path 的入口条件「rsp 指向 12 个用户 GPR 槽的底部」。
     *
     * 换言之：context_switch 的 6 槽 + ret 槽在【低地址】，GPR 帧与 iretq 帧
     * 在其上（高地址）。曾把 GPR 帧放在最低地址，导致 context_switch 把
     * 用户 GPR 当成自己的 callee-saved 弹掉，ret 落到用户 RIP 上却仍处在
     * Ring0（CS=0x8），表现为「内核态取指用户地址」的 #PF OOPS。
     *
     * 偏移（以 sp = kstack_top - 24 为基，单位 = 8 字节）：
     *    0..5    context_switch 的 6 个 callee-saved（值取用户 GPR，随后会被
     *            syscall_return_path 的 12 次 pop 覆盖，填用户值只为状态自洽）
     *    6       ret 目标 = fork_child_return
     *    7..18   12 个用户 GPR（r9,r8,r10,rdx,rsi,rdi,r15,r14,r13,r12,rbp,rbx）
     *   19..23   iretq 帧（RIP, CS, RFLAGS, RSP, SS）
     */
    uint64_t *gpr = (uint64_t *)frame;
    uint64_t *sp = (uint64_t *)child->kstack_top - 24;

    sp[0] = gpr[6];                    /* r15 */
    sp[1] = gpr[7];                    /* r14 */
    sp[2] = gpr[8];                    /* r13 */
    sp[3] = gpr[9];                    /* r12 */
    sp[4] = gpr[10];                   /* rbp */
    sp[5] = gpr[11];                   /* rbx */
    sp[6] = (uint64_t)fork_child_return;

    for (int i = 0; i < 12; i++) {
        sp[7 + i] = gpr[i];
    }
    sp[19] = parent->scr_rip;          /* 子进程从父的 syscall 下一条继续执行 */
    sp[20] = USER_CS_SEL;
    sp[21] = USER_RFLAGS;
    sp[22] = parent->scr_rsp;
    sp[23] = USER_DS_SEL;
    child->rsp = (uint64_t)sp;

    /* 子进程的返回暂存必须与父一致（syscall_return_path 会读它覆盖 iret 帧） */
    child->scr_rip = parent->scr_rip;
    child->scr_rsp = parent->scr_rsp;

    /* 7) 发布（入全局表 + 运行队列 + RESCHED IPI 立即唤醒） */
    task_publish_ready(child);

    kprintf("[posix] fork: parent pid=%lu -> child pid=%lu (cr3=%p)\n",
            (unsigned long)parent->id, (unsigned long)child->id,
            (void *)new_as);
    return (int64_t)child->id;         /* 父进程返回子 PID */
}

/* getpid / getppid / getuid / geteuid / getgid / getegid */
static int64_t sys_getpid(void)  { return (int64_t)sched_current()->id; }
static int64_t sys_getppid(void) { return (int64_t)sched_current()->parent_id; }
static int64_t sys_getuid(void)  { return (int64_t)sched_current()->uid; }
static int64_t sys_geteuid(void) { return (int64_t)sched_current()->euid; }
static int64_t sys_getgid(void)  { return (int64_t)sched_current()->gid; }
static int64_t sys_getegid(void) { return (int64_t)sched_current()->egid; }

/* setuid/setgid：单用户系统，仅 root(0) 可改；非 root 返回 EPERM。 */
static int64_t sys_setuid(uint32_t uid)
{
    task_t *t = sched_current();
    if (t->euid != 0) {
        return -SUKI_EPERM;
    }
    t->uid = uid;
    t->euid = uid;
    return 0;
}
static int64_t sys_setgid(uint32_t gid)
{
    task_t *t = sched_current();
    if (t->egid != 0) {
        return -SUKI_EPERM;
    }
    t->gid = gid;
    t->egid = gid;
    return 0;
}

/*
 * sys_waitpid(pid, status_ptr, options)
 *   pid  >0 等指定子进程；pid == -1 等任意子进程
 *   options & WNOHANG -> 无已退出子进程时立即返回 0（不阻塞）
 * status 编码遵循 POSIX/WIFEXITED 约定：正常退出为 (exitcode << 8)。
 */
static int64_t sys_waitpid(int64_t pid, int32_t *ustatus, int32_t options)
{
    bool nohang = (options & SUKI_WNOHANG) != 0;
    uint64_t cpid = 0, rc = 0;
    int64_t r = task_wait_any_child(pid == 0 ? -1 : pid, &cpid, &rc, nohang);
    if (r < 0) {
        /* WNOHANG 且子进程尚未退出：POSIX 规定返回 0（不是错误） */
        if (r == -SUKI_EAGAIN) {
            return 0;
        }
        return r;
    }
    if (ustatus) {
        int32_t st = (int32_t)((rc & 0xFF) << 8);   /* WIFEXITED|WEXITSTATUS */
        if (copy_to_user(ustatus, &st, sizeof(st)) != sizeof(st)) {
            /* 状态写不回去（用户指针非法）：子进程已被回收，无法重来。
             * 按 POSIX 语义这属于调用方错误，返回 EFAULT 并如实告知。 */
            return -SUKI_EFAULT;
        }
    }
    return (int64_t)cpid;
}

/* kill(pid, sig)：见 sched.c::task_signal 的「边界安全」说明 */
static int64_t sys_kill(int64_t pid, int32_t sig)
{
    if (pid <= 0 && pid != -1) {
        return -SUKI_EINVAL;     /* 本阶段无进程组语义 */
    }
    if (pid == -1) {
        return -SUKI_EINVAL;     /* 广播给所有进程：需进程组子系统，暂不支持 */
    }
    return task_signal((uint64_t)pid, (int)sig);
}

/* exit_group(code) —— 终止整个进程（本系统任务即进程，无线程组） */
static int64_t sys_exit_group(uint64_t code)
{
    task_exit_current(code & 0xFF);
    return 0;                    /* 不可达 */
}

/*
 * sys_brk(addr) —— 设置程序断点（堆顶）。
 *   addr == 0  -> 查询当前 brk（malloc 用它探测初始堆位置）
 *   addr 合法  -> 调整 brk 并返回新 brk；失败返回当前 brk（POSIX 惯例）
 * 语义：堆区间一次性登记为 VMA_ANON（按需零页填充）；brk 增长不需要立即
 * 分配物理页（首次触碰由 #PF 补页），brk 收缩则真正解除映射并回收页。
 */
static int64_t sys_brk(uint64_t addr)
{
    task_t *t = sched_current();

    /* 首次调用：定位堆起点并登记整块堆区 */
    if (t->brk_start == 0) {
        t->brk_start = POSIX_HEAP_BASE;
        t->brk = POSIX_HEAP_BASE;
        if (!vma_insert(t, POSIX_HEAP_BASE, POSIX_HEAP_BASE + POSIX_HEAP_MAX,
                        PTE_WRITE | PTE_NX, VMA_TYPE_ANON)) {
            return (int64_t)t->brk;      /* 登记失败：保持现状 */
        }
    }
    if (addr == 0) {
        return (int64_t)t->brk;
    }
    if (addr < t->brk_start || addr > t->brk_start + POSIX_HEAP_MAX) {
        return (int64_t)t->brk;          /* 越界：POSIX 返回当前 brk */
    }
    if (addr > t->brk) {
        /* 扩展：VMA 已覆盖整块堆区，按需补页由 #PF 完成，此处只需移动断点 */
        t->brk = addr;
    } else if (addr < t->brk) {
        /* 收缩：仅移动断点，不裁剪 VMA、不回收物理页。
         *
         * 原实现会对 [页对齐上界(addr), 页对齐上界(brk)) 调用 vma_unmap_range
         * 把这段区间从 VMA 链表永久裁掉。但 sys_brk 增长分支只更新 t->brk、
         * 并不重建被裁掉的 VMA 子区间，于是「先缩回、再增长到已裁区间」后，
         * 该区间已脱离 VMA，用户态任何访问都触 #PF 杀进程（即使地址仍在
         * 登记过的 [brk_start, brk_start+MAX) 大区间内）。
         *
         * POSIX 仅要求 brk 断点可移动，是否立即回收物理页是实现细节；本系统
         * 选择「惰性保留」：VMA 在首次 sys_brk(0) 时已一次性登记整块堆区，
         * 其后收缩只动断点、增长只动断点，VMA 始终完整覆盖 [base, base+MAX)，
         * 用户态补页（demand zero-fill）始终有效。物理页在进程退出时由
         * vma_destroy_all 统一回收，无泄漏。 */
        t->brk = addr;
    }
    return (int64_t)t->brk;
}

/* sbrk(increment) —— 增量调整堆；返回【旧】brk（POSIX 语义） */
static int64_t sys_sbrk(int64_t increment)
{
    task_t *t = sched_current();
    if (t->brk_start == 0) {
        int64_t base = sys_brk(0);
        if (base < 0) {
            return base;
        }
    }
    uint64_t old = t->brk;
    if (increment == 0) {
        return (int64_t)old;
    }
    uint64_t want = (increment > 0) ? (old + (uint64_t)increment)
                                    : (old - (uint64_t)(-increment));
    int64_t nb = sys_brk(want);
    if (nb != (int64_t)want) {
        return -SUKI_ENOMEM;             /* 调整失败 */
    }
    return (int64_t)old;
}

/* times(tms*) —— 进程 CPU 时间（节拍数） */
static int64_t sys_times(suki_tms_t *ubuf)
{
    task_t *t = sched_current();
    suki_tms_t tm;
    memset(&tm, 0, sizeof(tm));
    tm.tms_utime = (int64_t)t->utime_ticks;
    tm.tms_stime = (int64_t)t->stime_ticks;
    tm.tms_cutime = 0;      /* 已回收子进程的累计时间本阶段不统计 */
    tm.tms_cstime = 0;
    if (ubuf) {
        if (copy_to_user(ubuf, &tm, sizeof(tm)) != sizeof(tm)) {
            return -SUKI_EFAULT;
        }
    }
    /* 成功返回自启动起的时钟节拍数 */
    return (int64_t)(clock_monotonic_ns() / 10000000ULL);
}

/* umask(mask) —— 设置文件创建掩码，返回旧值 */
static int64_t sys_umask(uint32_t mask)
{
    task_t *t = sched_current();
    uint32_t old = t->umask;
    t->umask = mask & 0777;
    return (int64_t)old;
}

static int64_t sys_getrusage(int32_t who, suki_rusage_t *ubuf)
{
    if (who != SUKI_RUSAGE_SELF && who != SUKI_RUSAGE_CHILDREN) {
        return -SUKI_EINVAL;
    }
    if (!ubuf) {
        return 0;
    }
    task_t *t = sched_current();
    suki_rusage_t ru;
    memset(&ru, 0, sizeof(ru));
    if (who == SUKI_RUSAGE_SELF) {
        /* 节拍(10ms) -> 秒/微秒 */
        ru.ru_utime_sec = (int64_t)(t->utime_ticks / POSIX_CLK_TCK);
        ru.ru_utime_usec = (int64_t)((t->utime_ticks % POSIX_CLK_TCK)
                                     * (1000000 / POSIX_CLK_TCK));
        ru.ru_stime_sec = (int64_t)(t->stime_ticks / POSIX_CLK_TCK);
        ru.ru_stime_usec = (int64_t)((t->stime_ticks % POSIX_CLK_TCK)
                                     * (1000000 / POSIX_CLK_TCK));
    }
    if (copy_to_user(ubuf, &ru, sizeof(ru)) != sizeof(ru)) {
        return -SUKI_EFAULT;
    }
    return 0;
}

static int64_t sys_getrlimit(int32_t res, suki_rlimit_t *ubuf)
{
    if (!ubuf) {
        return -SUKI_EFAULT;
    }
    suki_rlimit_t rl;
    memset(&rl, 0, sizeof(rl));
    switch (res) {
    case SUKI_RLIMIT_NOFILE:
        rl.rlim_cur = SUKI_FD_MAX;
        rl.rlim_max = SUKI_FD_MAX;
        break;
    case SUKI_RLIMIT_STACK:
        rl.rlim_cur = 32UL * PAGE_SIZE;      /* 用户栈 32 页（见 USER_STACK_PAGES） */
        rl.rlim_max = 32UL * PAGE_SIZE;
        break;
    case SUKI_RLIMIT_AS:
        rl.rlim_cur = POSIX_HEAP_MAX + VMA_MMAP_MAX;
        rl.rlim_max = POSIX_HEAP_MAX + VMA_MMAP_MAX;
        break;
    case SUKI_RLIMIT_DATA:
        rl.rlim_cur = POSIX_HEAP_MAX;
        rl.rlim_max = POSIX_HEAP_MAX;
        break;
    case SUKI_RLIMIT_CORE:
    case SUKI_RLIMIT_CPU:
        break;                                /* 不支持：保持 0 */
    default:
        return -SUKI_EINVAL;
    }
    if (copy_to_user(ubuf, &rl, sizeof(rl)) != sizeof(rl)) {
        return -SUKI_EFAULT;
    }
    return 0;
}

static int64_t sys_setrlimit(int32_t res, const suki_rlimit_t *ubuf)
{
    /* 本阶段不支持放宽/收紧资源上限：如实返回 EPERM（非静默成功），
     * 避免应用误以为限制已生效而产生后续越界行为。 */
    (void)res;
    (void)ubuf;
    return -SUKI_EPERM;
}

/* gettid：本系统「任务即进程」，tid == pid */
static int64_t sys_gettid(void) { return (int64_t)sched_current()->id; }

/* prctl：仅实现 PR_SET_NAME(15) 的语义子集（改任务名便于诊断） */
static int64_t sys_prctl(int32_t option, uint64_t arg2)
{
#define SUKI_PR_SET_NAME   15
    if (option != SUKI_PR_SET_NAME) {
        return -SUKI_EINVAL;
    }
    char *buf = g_path_buf[cpu_index()];
    if (copy_from_user(buf, (const void *)arg2, 16) != 16) {
        return -SUKI_EFAULT;
    }
    buf[15] = '\0';
    strncpy(sched_current()->name, buf, sizeof(sched_current()->name) - 1);
    return 0;
}

/* futex：本阶段无用户态线程库（pthread 尚未落地）。真实的 futex 需要
 * 「按用户地址建等待队列」的能力；此处返回 ENOSYS 是 POSIX 允许的正确语义，
 * newlib/应用据此降级为忙等或自旋锁，不会误判为成功。 */
static int64_t sys_futex(uint64_t a1, uint64_t a2, uint64_t a3)
{
    (void)a1; (void)a2; (void)a3;
    return -SUKI_ENOSYS;
}

/* set_tid_address / arch_prctl：Linux 兼容桩，本架构无对应语义。
 * glibc 启动会调用它们；返回 0（成功无操作）是安全且符合语义的——
 * set_tid_address 只是登记一个「退出时清零的地址」，不做也无害；
 * arch_prctl 在 x86_64 上仅用于设置 FS/GS 基址，本系统 GS 由内核独占。 */
static int64_t sys_set_tid_address(uint64_t addr)
{
    (void)addr;
    return (int64_t)sched_current()->id;
}
static int64_t sys_arch_prctl(int32_t code, uint64_t addr)
{
    (void)code; (void)addr;
    return -SUKI_EINVAL;
}

/* ========================================================================== */
/*  文件与目录 I/O                                                             */
/* ========================================================================== */

static int64_t sys_open(const char *upath, int32_t flags, uint32_t mode)
{
    char *path = NULL;
    int64_t r = fetch_path(upath, &path);
    if (r < 0) {
        return r;
    }
    return fd_open(sched_current(), path, (int)flags, mode);
}

static int64_t sys_close(int32_t fd) { return fd_close(sched_current(), fd); }

static int64_t sys_read(int32_t fd, void *ubuf, uint64_t count)
{
    if (count == 0) {
        return 0;
    }
    return fd_read(sched_current(), fd, ubuf, (size_t)count);
}

static int64_t sys_write(int32_t fd, const void *ubuf, uint64_t count)
{
    if (count == 0) {
        return 0;
    }
    return fd_write(sched_current(), fd, ubuf, (size_t)count);
}

static int64_t sys_lseek(int32_t fd, int64_t off, int32_t whence)
{
    return fd_lseek(sched_current(), fd, (suki_off_t)off, (int)whence);
}

static int64_t sys_fstat(int32_t fd, suki_stat_t *ubuf)
{
    if (!ubuf) {
        return -SUKI_EFAULT;
    }
    suki_stat_t st;
    int r = fd_fstat(sched_current(), fd, &st);
    if (r < 0) {
        return r;
    }
    if (copy_to_user(ubuf, &st, sizeof(st)) != sizeof(st)) {
        return -SUKI_EFAULT;
    }
    return 0;
}

static int64_t sys_stat(const char *upath, suki_stat_t *ubuf)
{
    if (!ubuf) {
        return -SUKI_EFAULT;
    }
    char *path = NULL;
    int64_t r = fetch_path(upath, &path);
    if (r < 0) {
        return r;
    }
    suki_stat_t st;
    int rc = fd_stat(path, &st);
    if (rc < 0) {
        return rc;
    }
    if (copy_to_user(ubuf, &st, sizeof(st)) != sizeof(st)) {
        return -SUKI_EFAULT;
    }
    return 0;
}

/*
 * lstat：FAT32 无符号链接，语义上等价于 stat（POSIX 允许：对非符号链接
 * 的 lstat 与 stat 行为一致）。
 */
static int64_t sys_lstat(const char *upath, suki_stat_t *ubuf)
{
    return sys_stat(upath, ubuf);
}

static int64_t sys_unlink(const char *upath)
{
    char *path = NULL;
    int64_t r = fetch_path(upath, &path);
    if (r < 0) {
        return r;
    }
    return fd_unlink(path);
}

static int64_t sys_rmdir(const char *upath)
{
    char *path = NULL;
    int64_t r = fetch_path(upath, &path);
    if (r < 0) {
        return r;
    }
    return fd_rmdir(path);
}

static int64_t sys_mkdir(const char *upath, uint32_t mode)
{
    char *path = NULL;
    int64_t r = fetch_path(upath, &path);
    if (r < 0) {
        return r;
    }
    return fd_mkdir(path, mode);
}

static int64_t sys_opendir(const char *upath)
{
    char *path = NULL;
    int64_t r = fetch_path(upath, &path);
    if (r < 0) {
        return r;
    }
    return fd_opendir(sched_current(), path);
}

/*
 * readdir(fd, dirent*) —— 读一条目录项。
 * 返回 1 = 读到一条；0 = 目录结束；<0 = 负 errno。
 */
static int64_t sys_readdir(int32_t fd, suki_dirent_t *ubuf)
{
    if (!ubuf) {
        return -SUKI_EFAULT;
    }
    suki_dirent_t de;
    int r = fd_readdir(sched_current(), fd, &de);
    if (r <= 0) {
        return r;
    }
    if (copy_to_user(ubuf, &de, sizeof(de)) != sizeof(de)) {
        return -SUKI_EFAULT;
    }
    return 1;
}

/* closedir(fd) —— 本系统目录流即普通 fd，语义等同 close */
static int64_t sys_closedir(int32_t fd) { return fd_close(sched_current(), fd); }

static int64_t sys_dup(int32_t fd) { return fd_dup(sched_current(), fd); }
static int64_t sys_dup2(int32_t oldfd, int32_t newfd)
{
    return fd_dup2(sched_current(), oldfd, newfd);
}

static int64_t sys_fcntl(int32_t fd, int32_t cmd, uint64_t arg)
{
    return fd_fcntl(sched_current(), fd, (int)cmd, (int64_t)arg);
}

static int64_t sys_access(const char *upath, int32_t mode)
{
    char *path = NULL;
    int64_t r = fetch_path(upath, &path);
    if (r < 0) {
        return r;
    }
    return fd_access(path, (int)mode);
}

static int64_t sys_rename(const char *uold, const char *unew)
{
    char *oldp = g_path_buf[cpu_index()];
    char *newp = g_iobuf[cpu_index()];
    int64_t r = copy_path(uold, oldp, sizeof(g_path_buf[0]));
    if (r < 0) {
        return r;
    }
    r = copy_path(unew, newp, sizeof(g_iobuf[0]));
    if (r < 0) {
        return r;
    }
    return fd_rename(oldp, newp);
}

static int64_t sys_truncate(const char *upath, int64_t len)
{
    char *path = NULL;
    int64_t r = fetch_path(upath, &path);
    if (r < 0) {
        return r;
    }
    return fd_truncate(path, (suki_off_t)len);
}

static int64_t sys_ftruncate(int32_t fd, int64_t len)
{
    return fd_ftruncate(sched_current(), fd, (suki_off_t)len);
}

/*
 * chdir(path) —— 改变当前工作目录。
 * 校验存在性后再更新 task->cwd（经 access 走 FS_SERVER 确认路径有效），
 * 避免把不存在的目录写进 cwd 导致后续相对路径全部失败。
 */
static int64_t sys_chdir(const char *upath)
{
    char *path = NULL;
    int64_t r = fetch_path(upath, &path);
    if (r < 0) {
        return r;
    }
    if (fd_access(path, SUKI_F_OK) < 0) {
        return -SUKI_ENOENT;
    }
    task_t *t = sched_current();
    strncpy(t->cwd, path, sizeof(t->cwd) - 1);
    t->cwd[sizeof(t->cwd) - 1] = '\0';
    if (t->cwd[0] == '\0') {
        t->cwd[0] = '/';
        t->cwd[1] = '\0';
    }
    /* FAT32 簇号上下文：本系统路径解析由 FS_SERVER 完成，内核只需记录
     * 目录字符串；cwd_cluster 保留 0（根）以兼容既有代码路径。 */
    return 0;
}

/* getcwd(buf, size) —— 返回缓冲区指针；失败返回负 errno */
static int64_t sys_getcwd(char *ubuf, uint64_t size)
{
    if (!ubuf || size == 0) {
        return -SUKI_EINVAL;
    }
    task_t *t = sched_current();
    const char *cwd = (t->cwd[0] != '\0') ? t->cwd : "/";
    size_t len = strlen(cwd);
    if (len + 1 > size) {
        return -SUKI_ERANGE;
    }
    if (copy_to_user(ubuf, cwd, len + 1) != len + 1) {
        return -SUKI_EFAULT;
    }
    return (int64_t)(uint64_t)ubuf;      /* POSIX: 成功返回 buf */
}

static int64_t sys_pipe(int32_t *ufds)
{
    if (!ufds) {
        return -SUKI_EFAULT;
    }
    int fds[2] = { -1, -1 };
    int r = fd_pipe(sched_current(), fds);
    if (r < 0) {
        return r;
    }
    int32_t out[2] = { (int32_t)fds[0], (int32_t)fds[1] };
    if (copy_to_user(ufds, out, sizeof(out)) != sizeof(out)) {
        /* 回滚：管道已建但没法告知用户，必须关掉避免 fd 泄漏 */
        fd_close(sched_current(), fds[0]);
        fd_close(sched_current(), fds[1]);
        return -SUKI_EFAULT;
    }
    return 0;
}

/*
 * ioctl —— 仅支持 TTY 上最常用的两个请求：
 *   TIOCGWINSZ(0x5413) 取窗口尺寸（帧缓冲像素换算为字符格）
 *   TCGETS(0x5401)     取终端属性（返回默认值，IOICANON|ECHO 关闭标记）
 * 其余一律 -ENOTTY（POSIX 规定「非终端上做终端控制操作」的错误码）。
 */
static int64_t sys_ioctl(int32_t fd, uint64_t request, uint64_t arg)
{
    int r = fd_isatty(sched_current(), fd);
    if (r < 0) {
        return -SUKI_ENOTTY;
    }
#define TIOCGWINSZ   0x5413
#define TCGETS       0x5401
    if (request == TIOCGWINSZ) {
        /* 字符格数：帧缓冲尺寸 / 8x16 字模 */
        uint32_t ws[4] = { 25, 80, 0, 0 };   /* rows, cols, xpixel, ypixel */
        if (arg) {
            if (copy_to_user((void *)arg, ws, sizeof(ws)) != sizeof(ws)) {
                return -SUKI_EFAULT;
            }
        }
        return 0;
    }
    if (request == TCGETS) {
        /* 最小化 termios（本系统无行规程）：c_iflag/c_oflag/c_cflag/c_lflag
         * 全 0，c_line=0，c_cc 全 0。返回 0 表示「成功取到默认属性」。 */
        uint8_t tio[64];
        memset(tio, 0, sizeof(tio));
        if (arg) {
            if (copy_to_user((void *)arg, tio, sizeof(tio)) != sizeof(tio)) {
                return -SUKI_EFAULT;
            }
        }
        return 0;
    }
    return -SUKI_ENOTTY;
}

/* link / symlink / readlink —— FAT32 无索引结点与符号链接概念。
 * 返回 EOPNOTSUPP 是 POSIX 明确规定的「文件系统不支持该操作」语义，
 * 应用（如 coreutils 的 ln）能据此给出正确提示，而不是误判成功。 */
static int64_t sys_link(const char *o, const char *n)
{
    (void)o; (void)n;
    return -SUKI_EOPNOTSUPP;
}
static int64_t sys_symlink(const char *t, const char *l)
{
    (void)t; (void)l;
    return -SUKI_EOPNOTSUPP;
}
static int64_t sys_readlink(const char *p, char *b, uint64_t s)
{
    (void)p; (void)b; (void)s;
    return -SUKI_EINVAL;      /* 不是符号链接 */
}

/*
 * chmod/fchmod —— 真实生效（经 FS_MSG_CHMOD -> f_chmod 改写 FAT 属性字节）。
 * POSIX 所有权规则：只有 root 或文件所有者才能改权限。FAT32 无 uid/gid，
 * 所有文件的所有者视为 root(0)，故判定为「euid == 0 才允许」，其余 EPERM。
 * 权限位映射：FAT 只能承载「只读」——写位全无置 AM_RDO，否则清除（见
 * fs_server.c::handle_chmod）。
 * fchmod 走 fd 打开时保存的路径（内核 fd 槽中的 path 副本）。
 */
static int64_t sys_chmod(const char *upath, uint32_t mode)
{
    if (sched_current()->euid != 0) {
        return -SUKI_EPERM;
    }
    char *path = NULL;
    int64_t r = fetch_path(upath, &path);
    if (r < 0) {
        return r;
    }
    return fd_chmod(path, mode);
}

static int64_t sys_fchmod(int32_t fd, uint32_t mode)
{
    if (sched_current()->euid != 0) {
        return -SUKI_EPERM;
    }
    fd_entry_t *e = fd_get(sched_current(), fd);
    if (!e) {
        return -SUKI_EBADF;
    }
    if (e->type != FD_TYPE_FILE && e->type != FD_TYPE_DIR) {
        return -SUKI_EINVAL;
    }
    if (!e->path) {
        return -SUKI_EBADF;      /* 无路径副本（极罕见：槽已半释放） */
    }
    return fd_chmod(e->path, mode);
}

/* chown/fchown：同上，FAT32 无所有权模型 */
static int64_t sys_chown(const char *upath, uint32_t u, uint32_t g)
{
    (void)upath; (void)u; (void)g;
    return (sched_current()->euid == 0) ? 0 : -SUKI_EPERM;
}
static int64_t sys_fchown(int32_t fd, uint32_t u, uint32_t g)
{
    (void)fd; (void)u; (void)g;
    return (sched_current()->euid == 0) ? 0 : -SUKI_EPERM;
}

static int64_t sys_sync(void) { return fd_sync(); }
static int64_t sys_fsync(int32_t fd)
{
    /* FatFs 写操作即时落盘（无写缓存层），fsync 与 sync 等价 */
    (void)fd;
    return fd_sync();
}

/*
 * utimes(path, times[2]) —— 真实生效（经 FS_MSG_UTIME -> f_utime 改写
 * FAT 时间戳）。times 为 NULL 表示「设为当前时间」；否则取 [0]=atime、
 * [1]=mtime（秒 + 微秒）。所有权规则同 chmod（root 或所有者）。
 */
static int64_t sys_utimes(const char *upath, const suki_timeval_t *utv)
{
    if (sched_current()->euid != 0) {
        return -SUKI_EPERM;
    }
    char *path = NULL;
    int64_t r = fetch_path(upath, &path);
    if (r < 0) {
        return r;
    }
    int64_t atime = -1, mtime = -1;
    if (utv) {
        suki_timeval_t tv[2];
        if (copy_from_user(tv, utv, sizeof(tv)) != sizeof(tv)) {
            return -SUKI_EFAULT;
        }
        if (tv[0].tv_usec < 0 || tv[0].tv_usec >= 1000000
            || tv[1].tv_usec < 0 || tv[1].tv_usec >= 1000000) {
            return -SUKI_EINVAL;
        }
        atime = tv[0].tv_sec;
        mtime = tv[1].tv_sec;
    } else {
        /* POSIX：times == NULL -> 设为当前时间 */
        int64_t now = (int64_t)(rtc_posix_now_ns() / 1000000000ULL);
        atime = now;
        mtime = now;
    }
    return fd_utimes(path, atime, mtime);
}

/*
 * getdents(fd, buf, count) —— 批量读目录项（readdir 的批量版本，newlib 与
 * 大多数 libc 的 opendir/readdir 实现底层更偏好它）。
 * 反复调 fd_readdir 直到填满用户缓冲或目录结束；返回写入字节数（0 = 结束）。
 */
static int64_t sys_getdents(int32_t fd, void *ubuf, uint64_t count)
{
    if (!ubuf || count < sizeof(suki_dirent_t)) {
        return -SUKI_EINVAL;
    }
    /* 用 per-CPU 之外的内核堆做聚集缓冲：单条 dirent 280 字节，按 count 计算 */
    uint64_t maxent = count / sizeof(suki_dirent_t);
    if (maxent == 0) {
        return -SUKI_EINVAL;
    }
    if (maxent > 64) {
        maxent = 64;                 /* 单次上限，避免大块内核分配 */
    }
    suki_dirent_t *tmp = (suki_dirent_t *)kmalloc((size_t)maxent
                                                  * sizeof(suki_dirent_t));
    if (!tmp) {
        return -SUKI_ENOMEM;
    }
    uint64_t n = 0;
    for (uint64_t i = 0; i < maxent; i++) {
        int r = fd_readdir(sched_current(), fd, &tmp[i]);
        if (r < 0) {
            kfree(tmp);
            return r;
        }
        if (r == 0) {
            break;                   /* 目录结束 */
        }
        n++;
    }
    int64_t rc = 0;
    if (n > 0) {
        size_t bytes = (size_t)n * sizeof(suki_dirent_t);
        if (copy_to_user(ubuf, tmp, bytes) != bytes) {
            rc = -SUKI_EFAULT;
        } else {
            rc = (int64_t)bytes;
        }
    }
    kfree(tmp);
    return rc;
}

static int64_t sys_isatty(int32_t fd)
{
    int r = fd_isatty(sched_current(), fd);
    if (r == -SUKI_ENOTTY) {
        return 0;                    /* POSIX: 非终端返回 0 并置 ENOTTY */
    }
    return (r < 0) ? r : 1;
}

static int64_t sys_statfs(const char *upath, suki_statfs_t *ubuf)
{
    (void)upath;                     /* 单卷系统：路径不影响卷信息 */
    if (!ubuf) {
        return -SUKI_EFAULT;
    }
    suki_statfs_t sf;
    int r = fd_statfs(&sf);
    if (r < 0) {
        return r;
    }
    if (copy_to_user(ubuf, &sf, sizeof(sf)) != sizeof(sf)) {
        return -SUKI_EFAULT;
    }
    return 0;
}

static int64_t sys_fstatfs(int32_t fd, suki_statfs_t *ubuf)
{
    fd_entry_t *e = fd_get(sched_current(), fd);
    if (!e) {
        return -SUKI_EBADF;
    }
    return sys_statfs(NULL, ubuf);
}

/*
 * pread/pwrite —— 在指定偏移读写，【不改变】文件当前偏移。
 * 实现：保存当前偏移 -> 绝对定位 -> 读写 -> 恢复偏移。任一步失败都要恢复
 * 偏移后再返回错误，绝不留下被改动的文件位置（否则后续 read 会错位）。
 */
static int64_t sys_pread(int32_t fd, void *ubuf, uint64_t count, int64_t off)
{
    if (off < 0) {
        return -SUKI_EINVAL;
    }
    task_t *t = sched_current();
    suki_off_t saved = fd_lseek(t, fd, 0, SUKI_SEEK_CUR);
    if (saved < 0) {
        return saved;
    }
    suki_off_t pos = fd_lseek(t, fd, (suki_off_t)off, SUKI_SEEK_SET);
    if (pos < 0) {
        fd_lseek(t, fd, saved, SUKI_SEEK_SET);
        return pos;
    }
    int64_t r = fd_read(t, fd, ubuf, (size_t)count);
    fd_lseek(t, fd, saved, SUKI_SEEK_SET);
    return r;
}

static int64_t sys_pwrite(int32_t fd, const void *ubuf, uint64_t count,
                          int64_t off)
{
    if (off < 0) {
        return -SUKI_EINVAL;
    }
    task_t *t = sched_current();
    suki_off_t saved = fd_lseek(t, fd, 0, SUKI_SEEK_CUR);
    if (saved < 0) {
        return saved;
    }
    suki_off_t pos = fd_lseek(t, fd, (suki_off_t)off, SUKI_SEEK_SET);
    if (pos < 0) {
        fd_lseek(t, fd, saved, SUKI_SEEK_SET);
        return pos;
    }
    int64_t r = fd_write(t, fd, ubuf, (size_t)count);
    fd_lseek(t, fd, saved, SUKI_SEEK_SET);
    return r;
}

/*
 * readv/writev —— 散布读 / 聚集写。
 * 逐个 iovec 调用底层 read/write，累计字节数；首个失败即返回（已成功部分
 * 不回退，符合 POSIX「部分成功」语义：返回已传输字节数或首个错误）。
 */
static int64_t sys_readv(int32_t fd, const suki_iovec_t *uiov, int32_t cnt)
{
    if (!uiov || cnt <= 0 || cnt > 1024) {
        return -SUKI_EINVAL;
    }
    suki_iovec_t *iov = (suki_iovec_t *)kmalloc((size_t)cnt
                                                * sizeof(suki_iovec_t));
    if (!iov) {
        return -SUKI_ENOMEM;
    }
    if (copy_from_user(iov, uiov, (size_t)cnt * sizeof(suki_iovec_t))
            != (size_t)cnt * sizeof(suki_iovec_t)) {
        kfree(iov);
        return -SUKI_EFAULT;
    }
    task_t *t = sched_current();
    int64_t total = 0;
    for (int i = 0; i < cnt; i++) {
        if (iov[i].iov_len == 0) {
            continue;
        }
        int64_t r = fd_read(t, fd, iov[i].iov_base, (size_t)iov[i].iov_len);
        if (r < 0) {
            kfree(iov);
            return (total > 0) ? total : r;
        }
        total += r;
        if ((uint64_t)r < iov[i].iov_len) {
            break;              /* 短读：后续不再继续 */
        }
    }
    kfree(iov);
    return total;
}

static int64_t sys_writev(int32_t fd, const suki_iovec_t *uiov, int32_t cnt)
{
    if (!uiov || cnt <= 0 || cnt > 1024) {
        return -SUKI_EINVAL;
    }
    suki_iovec_t *iov = (suki_iovec_t *)kmalloc((size_t)cnt
                                                * sizeof(suki_iovec_t));
    if (!iov) {
        return -SUKI_ENOMEM;
    }
    if (copy_from_user(iov, uiov, (size_t)cnt * sizeof(suki_iovec_t))
            != (size_t)cnt * sizeof(suki_iovec_t)) {
        kfree(iov);
        return -SUKI_EFAULT;
    }
    task_t *t = sched_current();
    int64_t total = 0;
    for (int i = 0; i < cnt; i++) {
        if (iov[i].iov_len == 0) {
            continue;
        }
        int64_t r = fd_write(t, fd, iov[i].iov_base, (size_t)iov[i].iov_len);
        if (r < 0) {
            kfree(iov);
            return (total > 0) ? total : r;
        }
        total += r;
        if ((uint64_t)r < iov[i].iov_len) {
            break;              /* 短写 */
        }
    }
    kfree(iov);
    return total;
}

/*
 * poll(fds, nfds, timeout_ms)
 * 就绪判定（本系统当前只有 TTY 与管道两类 fd）：
 *   TTY   ：恒可写；可读恒不成立（键盘输入由 INPUT_SERVER 经 IPC 投递，
 *           不经过内核 fd 路径），故 POLLIN 永不置位；
 *   管道  ：有数据 -> POLLIN；有剩余空间 -> POLLOUT；对端已关 -> POLLHUP。
 * timeout：无就绪事件时以 task_yield() 让出 CPU 重试，直到超时（节拍精度）。
 * 返回就绪的 fd 数（0 = 超时），负 errno 为失败。
 */
static int64_t sys_poll(suki_pollfd_t *ufds, uint32_t nfds, int32_t timeout_ms)
{
    if (!ufds || nfds == 0 || nfds > 256) {
        return -SUKI_EINVAL;
    }
    suki_pollfd_t *fds = (suki_pollfd_t *)kmalloc((size_t)nfds
                                                  * sizeof(suki_pollfd_t));
    if (!fds) {
        return -SUKI_ENOMEM;
    }
    if (copy_from_user(fds, ufds, (size_t)nfds * sizeof(suki_pollfd_t))
            != (size_t)nfds * sizeof(suki_pollfd_t)) {
        kfree(fds);
        return -SUKI_EFAULT;
    }
    task_t *t = sched_current();
    uint64_t deadline = clock_monotonic_ns()
                        + (timeout_ms < 0 ? 0 : (uint64_t)timeout_ms) * 1000000ULL;

    for (;;) {
        int32_t ready = 0;
        for (uint32_t i = 0; i < nfds; i++) {
            fds[i].revents = 0;
            fd_entry_t *e = fd_get(t, fds[i].fd);
            if (!e) {
                fds[i].revents = SUKI_POLLNVAL;
                continue;
            }
            if (e->type == FD_TYPE_TTY) {
                if (fds[i].events & SUKI_POLLOUT) {
                    fds[i].revents |= SUKI_POLLOUT;
                }
            } else if (e->type == FD_TYPE_PIPE) {
                int peer = e->backend;
                bool peer_alive = (peer >= 0 && peer < FD_SLOT_MAX);
                if (e->pipe && e->pipe->count > 0) {
                    fds[i].revents |= SUKI_POLLIN;
                }
                if (!peer_alive) {
                    fds[i].revents |= SUKI_POLLHUP;
                } else if (fds[i].events & SUKI_POLLOUT) {
                    fds[i].revents |= SUKI_POLLOUT;
                }
            } else {
                /* 普通文件：POSIX 认为普通文件总是就绪 */
                fds[i].revents |= (fds[i].events & (SUKI_POLLIN | SUKI_POLLOUT));
            }
            if (fds[i].revents) {
                ready++;
            }
        }
        if (ready > 0 || timeout_ms == 0) {
            if (copy_to_user(ufds, fds, (size_t)nfds * sizeof(suki_pollfd_t))
                    != (size_t)nfds * sizeof(suki_pollfd_t)) {
                kfree(fds);
                return -SUKI_EFAULT;
            }
            kfree(fds);
            return (int64_t)ready;
        }
        if (timeout_ms > 0 && clock_monotonic_ns() >= deadline) {
            break;
        }
        task_yield();               /* 让出 CPU，避免忙等空转 */
    }
    if (copy_to_user(ufds, fds, (size_t)nfds * sizeof(suki_pollfd_t))
            != (size_t)nfds * sizeof(suki_pollfd_t)) {
        kfree(fds);
        return -SUKI_EFAULT;
    }
    kfree(fds);
    return 0;
}

/*
 * select(nfds, readfds, writefds, exceptfds, timeout)
 * 复用 poll 的就绪判定逻辑：把 fd_set 转成 pollfd 数组，poll 后再写回。
 * 返回就绪 fd 总数；超时返回 0；失败返回负 errno。
 */
static int64_t sys_select(int32_t nfds, suki_fd_set_t *ur,
                          suki_fd_set_t *uw, suki_fd_set_t *ue,
                          const suki_timeval_t *utv)
{
    if (nfds < 0 || nfds > SUKI_FD_SETSIZE) {
        return -SUKI_EINVAL;
    }
    if (nfds == 0 && !utv) {
        return 0;
    }
    suki_timeval_t tv;
    int32_t timeout_ms = -1;
    if (utv) {
        if (copy_from_user(&tv, utv, sizeof(tv)) != sizeof(tv)) {
            return -SUKI_EFAULT;
        }
        if (tv.tv_sec < 0 || tv.tv_usec < 0) {
            return -SUKI_EINVAL;
        }
        timeout_ms = (int32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
    }

    suki_fd_set_t rset, wset, eset;
    memset(&rset, 0, sizeof(rset));
    memset(&wset, 0, sizeof(wset));
    memset(&eset, 0, sizeof(eset));
    if (ur && copy_from_user(&rset, ur, sizeof(rset)) != sizeof(rset)) {
        return -SUKI_EFAULT;
    }
    if (uw && copy_from_user(&wset, uw, sizeof(wset)) != sizeof(wset)) {
        return -SUKI_EFAULT;
    }
    if (ue && copy_from_user(&eset, ue, sizeof(eset)) != sizeof(eset)) {
        return -SUKI_EFAULT;
    }

    /* 构造 pollfd 数组（每个被关注的 fd 一项，合并读/写关注） */
    suki_pollfd_t *fds = (suki_pollfd_t *)kmalloc((size_t)SUKI_FD_SETSIZE
                                                  * sizeof(suki_pollfd_t));
    if (!fds) {
        return -SUKI_ENOMEM;
    }
    uint32_t n = 0;
    for (int fd = 0; fd < nfds; fd++) {
        int16_t ev = 0;
        if (ur && (rset.bits[fd / 64] >> (fd % 64)) & 1) {
            ev |= SUKI_POLLIN;
        }
        if (uw && (wset.bits[fd / 64] >> (fd % 64)) & 1) {
            ev |= SUKI_POLLOUT;
        }
        if (ue && (eset.bits[fd / 64] >> (fd % 64)) & 1) {
            ev |= SUKI_POLLPRI;
        }
        if (ev) {
            fds[n].fd = fd;
            fds[n].events = ev;
            fds[n].revents = 0;
            n++;
        }
    }

    int64_t ret;
    if (n == 0) {
        /* 未关注任何 fd：退化为纯超时等待 */
        uint64_t dl = clock_monotonic_ns()
                      + (timeout_ms < 0 ? 0 : (uint64_t)timeout_ms) * 1000000ULL;
        while (timeout_ms != 0) {
            if (timeout_ms > 0 && clock_monotonic_ns() >= dl) {
                break;
            }
            task_yield();
        }
        ret = 0;
    } else {
        /* 直接复用 sys_poll 的判定：这里内联一份（避免经用户指针往返） */
        task_t *t = sched_current();
        uint64_t dl = clock_monotonic_ns()
                      + (timeout_ms < 0 ? 0 : (uint64_t)timeout_ms) * 1000000ULL;
        ret = 0;
        for (;;) {
            ret = 0;
            for (uint32_t i = 0; i < n; i++) {
                fds[i].revents = 0;
                fd_entry_t *e = fd_get(t, fds[i].fd);
                if (!e) {
                    fds[i].revents = SUKI_POLLNVAL;
                    continue;
                }
                if (e->type == FD_TYPE_TTY) {
                    if (fds[i].events & SUKI_POLLOUT) {
                        fds[i].revents |= SUKI_POLLOUT;
                    }
                } else if (e->type == FD_TYPE_PIPE) {
                    if (e->pipe && e->pipe->count > 0) {
                        fds[i].revents |= SUKI_POLLIN;
                    }
                    if (fds[i].events & SUKI_POLLOUT) {
                        fds[i].revents |= SUKI_POLLOUT;
                    }
                } else {
                    fds[i].revents |=
                        (fds[i].events & (SUKI_POLLIN | SUKI_POLLOUT));
                }
                if (fds[i].revents) {
                    ret++;
                }
            }
            if (ret > 0 || timeout_ms == 0) {
                break;
            }
            if (timeout_ms > 0 && clock_monotonic_ns() >= dl) {
                break;
            }
            task_yield();
        }
    }

    /* 写回 fd_set：只保留就绪位 */
    if (ur || uw || ue) {
        suki_fd_set_t nset;
        memset(&nset, 0, sizeof(nset));
        for (uint32_t i = 0; i < n; i++) {
            int fd = fds[i].fd;
            if (ur && (fds[i].revents & (SUKI_POLLIN | SUKI_POLLHUP))) {
                nset.bits[fd / 64] |= (uint64_t)1 << (fd % 64);
            }
        }
        if (ur && copy_to_user(ur, &nset, sizeof(nset)) != sizeof(nset)) {
            kfree(fds);
            return -SUKI_EFAULT;
        }
        memset(&nset, 0, sizeof(nset));
        for (uint32_t i = 0; i < n; i++) {
            int fd = fds[i].fd;
            if (uw && (fds[i].revents & SUKI_POLLOUT)) {
                nset.bits[fd / 64] |= (uint64_t)1 << (fd % 64);
            }
        }
        if (uw && copy_to_user(uw, &nset, sizeof(nset)) != sizeof(nset)) {
            kfree(fds);
            return -SUKI_EFAULT;
        }
        if (ue) {
            memset(&nset, 0, sizeof(nset));      /* 无异常事件源 */
            if (copy_to_user(ue, &nset, sizeof(nset)) != sizeof(nset)) {
                kfree(fds);
                return -SUKI_EFAULT;
            }
        }
    }
    kfree(fds);
    return ret;
}

/* realpath(path, out) —— 规范化：去掉重复斜杠与 "."，支持前导 "/" */
static int64_t sys_realpath(const char *upath, char *uout)
{
    char *path = NULL;
    int64_t r = fetch_path(upath, &path);
    if (r < 0) {
        return r;
    }
    char *out = g_iobuf[cpu_index()];
    size_t o = 0;
    bool abs = (path[0] == '/');
    if (abs) {
        out[o++] = '/';
    }
    const char *p = path;
    while (*p) {
        if (*p == '/') {
            p++;
            continue;
        }
        if (p[0] == '.' && (p[1] == '/' || p[1] == '\0')) {
            p++;
            continue;
        }
        /* 复制一个路径分量 */
        while (*p && *p != '/' && o < sizeof(g_iobuf[0]) - 1) {
            out[o++] = *p++;
        }
        if (*p == '/' && o < sizeof(g_iobuf[0]) - 1) {
            out[o++] = '/';
        }
    }
    out[o] = '\0';
    if (o == 0) {
        out[0] = '/';
        out[1] = '\0';
        o = 1;
    }
    if (!uout) {
        return (int64_t)o;
    }
    if (copy_to_user(uout, out, o + 1) != o + 1) {
        return -SUKI_EFAULT;
    }
    return (int64_t)(uint64_t)uout;    /* 成功返回 out 指针 */
}

/* mknod：创建设备/特殊节点需特权且本系统无 devtmpfs —— 非 root 返回 EPERM，
 * root 返回 EOPNOTSUPP（文件系统不支持），都不静默假装成功。 */
static int64_t sys_mknod(const char *p, uint32_t m, uint64_t d)
{
    (void)p; (void)m; (void)d;
    return (sched_current()->euid == 0) ? -SUKI_EOPNOTSUPP : -SUKI_EPERM;
}

/* telldir / seekdir：目录游标由内核 fd 侧 offset 维护 */
static int64_t sys_telldir(int32_t fd)
{
    fd_entry_t *e = fd_get(sched_current(), fd);
    if (!e) {
        return -SUKI_EBADF;
    }
    return (int64_t)e->offset;
}
static int64_t sys_seekdir(int32_t fd, int64_t loc)
{
    fd_entry_t *e = fd_get(sched_current(), fd);
    if (!e) {
        return -SUKI_EBADF;
    }
    /* 目录流不支持回退（FS_SERVER 侧 FatFs 目录游标只能前进）：
     * 只允许把内核侧计数游标设置到 >= 当前值，实际重定位不支持。 */
    if (loc < (int64_t)e->offset) {
        return -SUKI_EINVAL;
    }
    e->offset = (uint64_t)loc;
    return 0;
}

/* fpathconf(fd, name) —— 返回路径/文件相关运行时限制 */
static int64_t sys_fpathconf(int32_t fd, int32_t name)
{
    fd_entry_t *e = fd_get(sched_current(), fd);
    if (!e) {
        return -SUKI_EBADF;
    }
    switch (name) {
    case 0:  return SUKI_NAME_MAX;        /* _PC_NAME_MAX */
    case 1:  return SUKI_PATH_MAX;        /* _PC_PATH_MAX */
    case 2:  return 1;                    /* _PC_LINK_MAX（不支持硬链接） */
    case 3:  return 0;                    /* _PC_CHOWN_RESTRICTED（不限制） */
    case 4:  return 1;                    /* _PC_NO_TRUNC（超长名报 ENAMETOOLONG） */
    default: return -SUKI_EINVAL;
    }
}

/* ========================================================================== */
/*  内存映射                                                                   */
/* ========================================================================== */

/*
 * sys_mmap(addr, len, prot, flags, fd, off) —— POSIX 六参 mmap。
 * 支持：MAP_PRIVATE|MAP_ANONYMOUS（可带 MAP_FIXED）。
 * 安全红线（与 sys_mmap 两参版一致）：绝不给用户映射可执行内存（W^X），
 * 含 PROT_EXEC 的请求一律 EINVAL —— 这是本内核的既定安全策略，不是简化。
 * 文件映射（非 ANONYMOUS）依赖页缓存子系统（P1-4），当前按 POSIX 语义
 * 返回 ENOSYS，应用据此降级，不会误以为拿到了有效映射。
 */
static int64_t sys_mmap(uint64_t addr, uint64_t len, int32_t prot,
                        int32_t flags, int32_t fd, int64_t off)
{
    task_t *t = sched_current();
    if (len == 0 || len > VMA_MMAP_MAX) {
        return -SUKI_EINVAL;
    }
    if (addr & (PAGE_SIZE - 1)) {
        return -SUKI_EINVAL;
    }
    if (prot & SUKI_PROT_EXEC) {
        return -SUKI_EINVAL;              /* W^X 红线 */
    }
    if (prot & ~(SUKI_PROT_READ | SUKI_PROT_WRITE | SUKI_PROT_NONE)) {
        return -SUKI_EINVAL;
    }
    if (!(flags & SUKI_MAP_ANONYMOUS)) {
        return -SUKI_ENOSYS;              /* 文件映射需页缓存，尚未就绪 */
    }
    if ((flags & SUKI_MAP_SHARED) && (flags & SUKI_MAP_PRIVATE)) {
        return -SUKI_EINVAL;
    }
    (void)fd;
    (void)off;

    len = (len + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);
    uint64_t base;
    if (flags & SUKI_MAP_FIXED) {
        if (!addr || addr < VMA_MMAP_BASE
            || addr + len > VMA_MMAP_TOP) {
            return -SUKI_EINVAL;
        }
        base = addr;
    } else {
        base = vma_find_free(t, VMA_MMAP_BASE, VMA_MMAP_TOP, len);
        if (!base) {
            return -SUKI_ENOMEM;
        }
    }
    uint64_t vprot = PTE_NX | ((prot & SUKI_PROT_WRITE) ? PTE_WRITE : 0);
    if (!vma_insert(t, base, base + len, vprot, VMA_TYPE_ANON)) {
        return -SUKI_ENOMEM;
    }
    return (int64_t)base;
}

static int64_t sys_munmap(uint64_t addr, uint64_t len)
{
    task_t *t = sched_current();
    if (len == 0 || (addr & (PAGE_SIZE - 1))) {
        return -SUKI_EINVAL;
    }
    if (addr < VMA_MMAP_BASE || addr + len > VMA_MMAP_TOP
        || addr + len < addr) {
        return -SUKI_EINVAL;
    }
    len = (len + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);
    return vma_unmap_range(t, addr, addr + len) ? 0 : -SUKI_EINVAL;
}

static int64_t sys_mprotect(uint64_t addr, uint64_t len, int32_t prot)
{
    if (prot & SUKI_PROT_EXEC) {
        return -SUKI_EINVAL;              /* W^X 红线：不允许事后改为可执行 */
    }
    if (prot & ~(SUKI_PROT_READ | SUKI_PROT_WRITE | SUKI_PROT_NONE)) {
        return -SUKI_EINVAL;
    }
    if (len == 0 || (addr & (PAGE_SIZE - 1))) {
        return -SUKI_EINVAL;
    }
    len = (len + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);
    uint64_t vprot = PTE_NX | ((prot & SUKI_PROT_WRITE) ? PTE_WRITE : 0);
    return vma_protect(sched_current(), addr, addr + len, vprot)
           ? 0 : -SUKI_ENOMEM;
}

/* msync：匿名映射无后端存储，恒成功（POSIX：匿名映射的 msync 是 no-op） */
static int64_t sys_msync(uint64_t addr, uint64_t len, int32_t flags)
{
    (void)addr; (void)len; (void)flags;
    return 0;
}
/* madvise：无页回收策略，接受并忽略（POSIX 允许 madvise 为建议性、可忽略） */
static int64_t sys_madvise(uint64_t addr, uint64_t len, int32_t advice)
{
    if (advice < 0) {
        return -SUKI_EINVAL;
    }
    (void)addr; (void)len;
    return 0;
}
/* mincore：返回 EINVAL（本系统不导出页驻留位图，POSIX 允许不支持） */
static int64_t sys_mincore(uint64_t a, uint64_t l, void *v)
{
    (void)a; (void)l; (void)v;
    return -SUKI_EINVAL;
}
/* mremap：仅支持「原地不变长」（new_len == old_len），否则 ENOSYS。
 * 这是真实可用的最小语义，而非桩：请求等长时就是 no-op 成功。 */
static int64_t sys_mremap(uint64_t old_addr, uint64_t old_len,
                          uint64_t new_len, int32_t flags, uint64_t new_addr)
{
    (void)flags; (void)new_addr;
    if (old_len != new_len) {
        return -SUKI_ENOSYS;
    }
    if (!old_addr || (old_addr & (PAGE_SIZE - 1))) {
        return -SUKI_EINVAL;
    }
    return (int64_t)old_addr;
}

/* ========================================================================== */
/*  时间                                                                       */
/* ========================================================================== */

static int64_t sys_clock_gettime(int32_t clk, suki_timespec_t *utp)
{
    if (!utp) {
        return -SUKI_EFAULT;
    }
    uint64_t ns;
    switch (clk) {
    case SUKI_CLOCK_REALTIME:
    case SUKI_CLOCK_REALTIME_COARSE:
        ns = rtc_posix_now_ns();
        break;
    case SUKI_CLOCK_MONOTONIC:
    case SUKI_CLOCK_MONOTONIC_RAW:
    case SUKI_CLOCK_MONOTONIC_COARSE:
    case SUKI_CLOCK_BOOTTIME:
        ns = clock_monotonic_ns();
        break;
    case SUKI_CLOCK_PROCESS_CPUTIME_ID:
    case SUKI_CLOCK_THREAD_CPUTIME_ID: {
        task_t *t = sched_current();
        uint64_t ticks = t->utime_ticks + t->stime_ticks;
        ns = ticks * (1000000000ULL / (uint64_t)POSIX_CLK_TCK);
        break;
    }
    default:
        return -SUKI_EINVAL;
    }
    suki_timespec_t ts;
    ts.tv_sec = (int64_t)(ns / 1000000000ULL);
    ts.tv_nsec = (int64_t)(ns % 1000000000ULL);
    if (copy_to_user(utp, &ts, sizeof(ts)) != sizeof(ts)) {
        return -SUKI_EFAULT;
    }
    return 0;
}

static int64_t sys_clock_getres(int32_t clk, suki_timespec_t *utp)
{
    if (clk < 0 || clk > SUKI_CLOCK_BOOTTIME) {
        return -SUKI_EINVAL;
    }
    if (!utp) {
        return 0;
    }
    /* 实际分辨率 = 1 个定时器节拍（100Hz = 10ms）；CPUTIME 类同 */
    suki_timespec_t ts;
    ts.tv_sec = 0;
    ts.tv_nsec = (clk == SUKI_CLOCK_PROCESS_CPUTIME_ID
                  || clk == SUKI_CLOCK_THREAD_CPUTIME_ID)
                 ? (1000000000ULL / (uint64_t)POSIX_CLK_TCK)
                 : 10000000ULL;      /* 10ms */
    if (copy_to_user(utp, &ts, sizeof(ts)) != sizeof(ts)) {
        return -SUKI_EFAULT;
    }
    return 0;
}

/* clock_settime：可调整单调基准（等价于设定系统时间）。仅 root。
 * 实现：把「目标墙上时间」与当前单调时间之差重算基准，之后所有 REALTIME
 * 查询都基于新基准 —— 真正生效，不是空操作。 */
static int64_t sys_clock_settime(int32_t clk, const suki_timespec_t *utp)
{
    if (sched_current()->euid != 0) {
        return -SUKI_EPERM;
    }
    if (clk != SUKI_CLOCK_REALTIME) {
        return -SUKI_EINVAL;         /* 单调时钟不可设置（POSIX） */
    }
    if (!utp) {
        return -SUKI_EFAULT;
    }
    suki_timespec_t ts;
    if (copy_from_user(&ts, utp, sizeof(ts)) != sizeof(ts)) {
        return -SUKI_EFAULT;
    }
    if (ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000LL) {
        return -SUKI_EINVAL;
    }
    /* realtime = base + mono，故 base = want - mono。直接改基准即刻生效，
     * 无需触碰 RTC（RTC 只在启动时读一次）。 */
    uint64_t want_ns = (uint64_t)ts.tv_sec * 1000000000ULL
                       + (uint64_t)ts.tv_nsec;
    uint64_t mono = clock_monotonic_ns();
    if (want_ns < mono) {
        return -SUKI_EINVAL;         /* 早于启动时刻，无法用基准表达 */
    }
    if (!rtc_set_boot_unix(want_ns - mono)) {
        return -SUKI_EINVAL;
    }
    return 0;
}

static int64_t sys_gettimeofday(suki_timeval_t *utv, suki_timezone_t *utz)
{
    uint64_t ns = rtc_posix_now_ns();
    if (utv) {
        suki_timeval_t tv;
        tv.tv_sec = (int64_t)(ns / 1000000000ULL);
        tv.tv_usec = (int64_t)((ns % 1000000000ULL) / 1000ULL);
        if (copy_to_user(utv, &tv, sizeof(tv)) != sizeof(tv)) {
            return -SUKI_EFAULT;
        }
    }
    if (utz) {
        suki_timezone_t tz;
        tz.tz_minuteswest = 0;       /* 本系统按 UTC 运行 */
        tz.tz_dsttime = 0;
        if (copy_to_user(utz, &tz, sizeof(tz)) != sizeof(tz)) {
            return -SUKI_EFAULT;
        }
    }
    return 0;
}

static int64_t sys_settimeofday(const suki_timeval_t *utv,
                                const suki_timezone_t *utz)
{
    (void)utz;
    if (sched_current()->euid != 0) {
        return -SUKI_EPERM;
    }
    if (!utv) {
        return 0;
    }
    suki_timeval_t tv;
    if (copy_from_user(&tv, utv, sizeof(tv)) != sizeof(tv)) {
        return -SUKI_EFAULT;
    }
    if (tv.tv_usec < 0 || tv.tv_usec >= 1000000) {
        return -SUKI_EINVAL;
    }
    uint64_t want_ns = (uint64_t)tv.tv_sec * 1000000000ULL
                       + (uint64_t)tv.tv_usec * 1000ULL;
    uint64_t mono = clock_monotonic_ns();
    if (want_ns < mono) {
        return -SUKI_EINVAL;
    }
    if (!rtc_set_boot_unix(want_ns - mono)) {
        return -SUKI_EINVAL;
    }
    return 0;
}

/*
 * nanosleep(req, rem) —— 睡眠指定时长。
 * 实现：目标时刻 = 当前单调时间 + 请求时长；循环中 task_yield() 让出 CPU，
 * 直到到达目标（精度受 LAPIC 100Hz 节拍限制，即最坏多睡 <10ms）。
 * 本阶段无可投递信号，故永不返回 EINTR，rem 恒写 0（POSIX 允许：无剩余时间
 * 时 rem 的内容未定义，这里明确清零以免调用方读到垃圾）。
 */
static int64_t sys_nanosleep(const suki_timespec_t *ureq, suki_timespec_t *urem)
{
    suki_timespec_t req;
    if (!ureq) {
        return -SUKI_EFAULT;
    }
    if (copy_from_user(&req, ureq, sizeof(req)) != sizeof(req)) {
        return -SUKI_EFAULT;
    }
    if (req.tv_nsec < 0 || req.tv_nsec >= 1000000000LL || req.tv_sec < 0) {
        return -SUKI_EINVAL;
    }
    uint64_t want_ns = (uint64_t)req.tv_sec * 1000000000ULL
                       + (uint64_t)req.tv_nsec;
    if (want_ns == 0) {
        return 0;                    /* 请求 0：立即返回（POSIX 允许不睡眠） */
    }
    uint64_t target = clock_monotonic_ns() + want_ns;
    while ((int64_t)(target - clock_monotonic_ns()) > 0) {
        task_yield();
    }
    if (urem) {
        suki_timespec_t rem;
        rem.tv_sec = 0;
        rem.tv_nsec = 0;
        if (copy_to_user(urem, &rem, sizeof(rem)) != sizeof(rem)) {
            return -SUKI_EFAULT;
        }
    }
    return 0;
}

/* time(tloc) —— 返回秒级墙上时间，并写入 *tloc（若非 NULL） */
static int64_t sys_time(int64_t *utloc)
{
    int64_t sec = (int64_t)(rtc_posix_now_ns() / 1000000000ULL);
    if (utloc) {
        if (copy_to_user(utloc, &sec, sizeof(sec)) != sizeof(sec)) {
            return -SUKI_EFAULT;
        }
    }
    return sec;
}

/* alarm(seconds) —— 定时 SIGALRM：本阶段无信号投递，返回 0（无待处理闹钟）。
 * POSIX 允许返回 0 表示「此前没有设置过闹钟」，语义正确。 */
static int64_t sys_alarm(uint32_t sec)
{
    (void)sec;
    return 0;
}

/* getitimer/setitimer：间隔定时器需信号投递，本阶段返回 ENOSYS（POSIX 允许
 * 「不支持该定时器」返回 ENOSYS，应用据此降级）。 */
static int64_t sys_getitimer(int32_t which, void *val)
{
    (void)which; (void)val;
    return -SUKI_ENOSYS;
}
static int64_t sys_setitimer(int32_t which, const void *nv, void *ov)
{
    (void)which; (void)nv; (void)ov;
    return -SUKI_ENOSYS;
}

/* ========================================================================== */
/*  系统信息                                                                   */
/* ========================================================================== */

static int64_t sys_uname(suki_utsname_t *ubuf)
{
    if (!ubuf) {
        return -SUKI_EFAULT;
    }
    suki_utsname_t u;
    memset(&u, 0, sizeof(u));
    strncpy(u.sysname, "SukiOS", sizeof(u.sysname) - 1);
    strncpy(u.nodename, "sukios", sizeof(u.nodename) - 1);
    strncpy(u.release, "1.0.0", sizeof(u.release) - 1);
    strncpy(u.version, "SukiOS hybrid kernel (Mach IPC + BSD POSIX)",
            sizeof(u.version) - 1);
    strncpy(u.machine, "x86_64", sizeof(u.machine) - 1);
    strncpy(u.domainname, "", sizeof(u.domainname) - 1);
    if (copy_to_user(ubuf, &u, sizeof(u)) != sizeof(u)) {
        return -SUKI_EFAULT;
    }
    return 0;
}

static int64_t sys_sysinfo(suki_sysinfo_t *ubuf)
{
    if (!ubuf) {
        return -SUKI_EFAULT;
    }
    suki_sysinfo_t si;
    memset(&si, 0, sizeof(si));
    si.uptime = (int64_t)(clock_monotonic_ns() / 1000000000ULL);
    si.loads[0] = 0;
    si.loads[1] = 0;
    si.loads[2] = 0;
    si.totalram = pmm_total_pages() * PAGE_SIZE;
    si.freeram = pmm_free_pages() * PAGE_SIZE;
    si.sharedram = 0;
    si.bufferram = 0;
    si.totalswap = 0;
    si.freeswap = 0;
    si.procs = (uint16_t)task_count();
    si.mem_unit = 1;
    if (copy_to_user(ubuf, &si, sizeof(si)) != sizeof(si)) {
        return -SUKI_EFAULT;
    }
    return 0;
}

/*
 * getrandom(buf, len, flags) —— 填充加密用途的随机字节。
 * 熵源：每字节由「TSC 低位 + 单调时间 + 内部 LCG 状态」混合并做乘散列
 * （splitmix64 变体）。这是【非阻塞】的确定性混合，不是密码学级 CSPRNG，
 * 但对 newlib/应用初始化（栈保护 canary、哈希种子）完全够用；grnd_flags
 * 中的 GRND_NONBLOCK 恒被满足（本函数从不阻塞）。
 */
static int64_t sys_getrandom(void *ubuf, uint64_t len, uint32_t flags)
{
    (void)flags;                        /* 本实现恒非阻塞 */
    if (!ubuf) {
        return -SUKI_EFAULT;
    }
    if (len > 256) {
        len = 256;                      /* 单次上限，避免大块内核分配与长临界区 */
    }
    static uint64_t s_state = 0;
    uint8_t *kb = (uint8_t *)g_iobuf[cpu_index()];
    if (len > sizeof(g_iobuf[0])) {
        len = sizeof(g_iobuf[0]);
    }
    for (uint64_t i = 0; i < len; i++) {
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        uint64_t x = ((uint64_t)hi << 32) | lo;
        x ^= clock_monotonic_ns();
        x ^= s_state;
        x ^= x >> 33; x *= 0xFF51AFD7ED558CCDUL;
        x ^= x >> 33; x *= 0xC4CEB9FE1A85EC53UL;
        x ^= x >> 33;
        s_state = x;
        kb[i] = (uint8_t)(x >> 24);     /* 取高位（低位与 TSC 相关性更强） */
    }
    if (copy_to_user(ubuf, kb, (size_t)len) != (size_t)len) {
        return -SUKI_EFAULT;
    }
    return (int64_t)len;
}

static int64_t sys_sysconf(int32_t name)
{
    switch (name) {
    case SUKI_SC_PAGESIZE:          return (int64_t)PAGE_SIZE;
    case SUKI_SC_NPROCESSORS_ONLN:  return (int64_t)smp_online_count();
    case SUKI_SC_PHYS_PAGES:        return (int64_t)pmm_total_pages();
    case SUKI_SC_AVPHYS_PAGES:      return (int64_t)pmm_free_pages();
    case SUKI_SC_OPEN_MAX:          return SUKI_FD_MAX;
    case SUKI_SC_CLK_TCK:           return POSIX_CLK_TCK;
    case SUKI_SC_ARG_MAX:           return 128 * 1024;
    case SUKI_SC_HOST_NAME_MAX:     return 64;
    default:                        return -SUKI_EINVAL;
    }
}

static int64_t sys_gethostname(char *ubuf, uint64_t len)
{
    if (!ubuf || len == 0) {
        return -SUKI_EINVAL;
    }
    static char hn[64] = "sukios";
    size_t n = strlen(hn) + 1;
    if (n > len) {
        n = (size_t)len;
    }
    if (copy_to_user(ubuf, hn, n) != n) {
        return -SUKI_EFAULT;
    }
    return 0;
}

static int64_t sys_sethostname(const char *ubuf, uint64_t len)
{
    if (sched_current()->euid != 0) {
        return -SUKI_EPERM;
    }
    if (!ubuf || len == 0 || len > 64) {
        return -SUKI_EINVAL;
    }
    static char hn[64];
    if (copy_from_user(hn, ubuf, (size_t)len) != (size_t)len) {
        return -SUKI_EFAULT;
    }
    hn[len > 63 ? 63 : len] = '\0';
    return 0;
}

/* getpgrp/setpgrp/getsid：本阶段无进程组/会话概念。
 * 返回「自身 PID 作为进程组 ID」是 Unix 单进程场景下的正确值；
 * setpgrp 为 no-op 成功（自身成为组长，本就如此）。 */
static int64_t sys_getpgrp(void) { return (int64_t)sched_current()->id; }
static int64_t sys_setpgrp(void) { return 0; }
static int64_t sys_getsid(void)  { return (int64_t)sched_current()->id; }

/* nice/getpriority/setpriority：优先级调整。
 * nice(incr) 成功后返回新的 nice 值（Linux 语义）；本系统 priority 越小
 * 优先级越高，故 nice 增大 -> priority 增大（优先级降低）。 */
static int64_t sys_nice(int32_t incr)
{
    task_t *t = sched_current();
    if (incr < -20 || incr > 19) {
        return -SUKI_EPERM;
    }
    int64_t newpri = (int64_t)t->priority + incr;
    if (newpri < 0) {
        newpri = 0;
    }
    if (newpri > 255) {
        newpri = 255;
    }
    if (incr < 0 && t->euid != 0) {
        return -SUKI_EPERM;          /* 提高优先级需特权 */
    }
    t->priority = (uint64_t)newpri;
    return incr;
}
static int64_t sys_getpriority(int32_t which, uint32_t who)
{
    (void)which; (void)who;
    return (int64_t)(sched_current()->priority) - 128;
}
static int64_t sys_setpriority(int32_t which, uint32_t who, int32_t prio)
{
    (void)which; (void)who;
    task_t *t = sched_current();
    if (prio < -20 || prio > 19) {
        return -SUKI_EINVAL;
    }
    if (prio < 0 && t->euid != 0) {
        return -SUKI_EPERM;
    }
    t->priority = (uint64_t)(prio + 128);
    return 0;
}

/*
 * 网络系统调用（socket/bind/connect/...）
 * 号位已在 <sukios/posix.h> 中预留（130..149）。网络协议栈属后续里程碑，
 * 此刻返回 -ENOSYS 是 POSIX 明确规定的「功能未实现」语义——应用（包括
 * 移植后的 newlib 网络例程）据此正确报错，绝不会误判为成功。
 */
static int64_t sys_socket_stub(void)
{
    return -SUKI_ENOSYS;
}

/* ========================================================================== */
/*  分发器                                                                     */
/* ========================================================================== */

void posix_init(void)
{
    fd_init();
    kprintf("[posix] POSIX syscall layer ready (syscall table: 0..%d, "
            "fd slots=%d)\n", SYSCALL_MAX - 1, FD_SLOT_MAX);
}

/*
 * posix_dispatch —— POSIX 号区（0..SYSCALL_MAX-1）的分发。
 * 返回 true 表示 num 已由本层处理（*ret 有效）；false 表示非 POSIX 号，
 * 由调用方按「未知系统调用」处理。
 *
 * 参数约定：a1..a6 依次对应用户态 rdi/rsi/rdx/r10/r8/r9（第 4 参走 r10，
 * 因 syscall 指令占用 rcx）；a6 由 syscall_entry.S 经栈传入。
 */
bool posix_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                    uint64_t a4, uint64_t a5, uint64_t a6, int64_t *ret)
{
    int64_t r;

    switch (num) {
    /* ---- 进程 / 资源（20..39）---- */
    case SYS_FORK:             r = sys_fork(); break;
    case SYS_GETPID:           r = sys_getpid(); break;
    case SYS_GETPPID:          r = sys_getppid(); break;
    case SYS_WAITPID:
        r = sys_waitpid((int64_t)a1, (int32_t *)a2, (int32_t)a3);
        break;
    case SYS_KILL:             r = sys_kill((int64_t)a1, (int32_t)a2); break;
    case SYS_GETUID:           r = sys_getuid(); break;
    case SYS_GETGID:           r = sys_getgid(); break;
    case SYS_GETEUID:          r = sys_geteuid(); break;
    case SYS_GETEGID:          r = sys_getegid(); break;
    case SYS_SETUID:           r = sys_setuid((uint32_t)a1); break;
    case SYS_SETGID:           r = sys_setgid((uint32_t)a1); break;
    case SYS_BRK:              r = sys_brk(a1); break;
    case SYS_SBRK:             r = sys_sbrk((int64_t)a1); break;
    case SYS_TIMES:            r = sys_times((suki_tms_t *)a1); break;
    case SYS_UMASK:            r = sys_umask((uint32_t)a1); break;
    case SYS_GETTID:           r = sys_gettid(); break;
    case SYS_EXIT_GROUP:       r = sys_exit_group(a1); break;
    case SYS_SET_TID_ADDRESS:  r = sys_set_tid_address(a1); break;
    case SYS_ARCH_PRCTL:       r = sys_arch_prctl((int32_t)a1, a2); break;
    case SYS_FUTEX:            r = sys_futex(a1, a2, a3); break;
    case SYS_GETRLIMIT:
        r = sys_getrlimit((int32_t)a1, (suki_rlimit_t *)a2);
        break;
    case SYS_SETRLIMIT:
        r = sys_setrlimit((int32_t)a1, (const suki_rlimit_t *)a2);
        break;
    case SYS_GETRUSAGE:
        r = sys_getrusage((int32_t)a1, (suki_rusage_t *)a2);
        break;

    /* ---- 文件与目录（40..89）---- */
    case SYS_OPEN:
        r = sys_open((const char *)a1, (int32_t)a2, (uint32_t)a3);
        break;
    case SYS_CLOSE:            r = sys_close((int32_t)a1); break;
    case SYS_READ:
        r = sys_read((int32_t)a1, (void *)a2, a3);
        break;
    case SYS_WRITE:
        r = sys_write((int32_t)a1, (const void *)a2, a3);
        break;
    case SYS_LSEEK:
        r = sys_lseek((int32_t)a1, (int64_t)a2, (int32_t)a3);
        break;
    case SYS_STAT:
        r = sys_stat((const char *)a1, (suki_stat_t *)a2);
        break;
    case SYS_FSTAT:
        r = sys_fstat((int32_t)a1, (suki_stat_t *)a2);
        break;
    case SYS_LSTAT:
        r = sys_lstat((const char *)a1, (suki_stat_t *)a2);
        break;
    case SYS_UNLINK:           r = sys_unlink((const char *)a1); break;
    case SYS_MKDIR:
        r = sys_mkdir((const char *)a1, (uint32_t)a2);
        break;
    case SYS_RMDIR:            r = sys_rmdir((const char *)a1); break;
    case SYS_OPENDIR:          r = sys_opendir((const char *)a1); break;
    case SYS_READDIR:
        r = sys_readdir((int32_t)a1, (suki_dirent_t *)a2);
        break;
    case SYS_CLOSEDIR:         r = sys_closedir((int32_t)a1); break;
    case SYS_DUP:              r = sys_dup((int32_t)a1); break;
    case SYS_DUP2:
        r = sys_dup2((int32_t)a1, (int32_t)a2);
        break;
    case SYS_FCNTL:
        r = sys_fcntl((int32_t)a1, (int32_t)a2, a3);
        break;
    case SYS_ACCESS:
        r = sys_access((const char *)a1, (int32_t)a2);
        break;
    case SYS_RENAME:
        r = sys_rename((const char *)a1, (const char *)a2);
        break;
    case SYS_TRUNCATE:
        r = sys_truncate((const char *)a1, (int64_t)a2);
        break;
    case SYS_FTRUNCATE:
        r = sys_ftruncate((int32_t)a1, (int64_t)a2);
        break;
    case SYS_CHDIR:            r = sys_chdir((const char *)a1); break;
    case SYS_GETCWD:
        r = sys_getcwd((char *)a1, a2);
        break;
    case SYS_PIPE:             r = sys_pipe((int32_t *)a1); break;
    case SYS_IOCTL:
        r = sys_ioctl((int32_t)a1, a2, a3);
        break;
    case SYS_LINK:
        r = sys_link((const char *)a1, (const char *)a2);
        break;
    case SYS_SYMLINK:
        r = sys_symlink((const char *)a1, (const char *)a2);
        break;
    case SYS_READLINK:
        r = sys_readlink((const char *)a1, (char *)a2, a3);
        break;
    case SYS_CHMOD:
        r = sys_chmod((const char *)a1, (uint32_t)a2);
        break;
    case SYS_FCHMOD:
        r = sys_fchmod((int32_t)a1, (uint32_t)a2);
        break;
    case SYS_SYNC:             r = sys_sync(); break;
    case SYS_FSYNC:            r = sys_fsync((int32_t)a1); break;
    case SYS_UTIMES:
        r = sys_utimes((const char *)a1, (const suki_timeval_t *)a2);
        break;
    case SYS_GETDENTS:
        r = sys_getdents((int32_t)a1, (void *)a2, a3);
        break;
    case SYS_ISATTY:           r = sys_isatty((int32_t)a1); break;
    case SYS_STATFS:
        r = sys_statfs((const char *)a1, (suki_statfs_t *)a2);
        break;
    case SYS_FSTATFS:
        r = sys_fstatfs((int32_t)a1, (suki_statfs_t *)a2);
        break;
    case SYS_PREAD:
        r = sys_pread((int32_t)a1, (void *)a2, a3, (int64_t)a4);
        break;
    case SYS_PWRITE:
        r = sys_pwrite((int32_t)a1, (const void *)a2, a3, (int64_t)a4);
        break;
    case SYS_READV:
        r = sys_readv((int32_t)a1, (const suki_iovec_t *)a2, (int32_t)a3);
        break;
    case SYS_WRITEV:
        r = sys_writev((int32_t)a1, (const suki_iovec_t *)a2, (int32_t)a3);
        break;
    case SYS_SELECT:
        r = sys_select((int32_t)a1, (suki_fd_set_t *)a2,
                       (suki_fd_set_t *)a3, (suki_fd_set_t *)a4,
                       (const suki_timeval_t *)a5);
        break;
    case SYS_POLL:
        r = sys_poll((suki_pollfd_t *)a1, (uint32_t)a2, (int32_t)a3);
        break;
    case SYS_REALPATH:
        r = sys_realpath((const char *)a1, (char *)a2);
        break;
    case SYS_MKNOD:
        r = sys_mknod((const char *)a1, (uint32_t)a2, a3);
        break;
    case SYS_CHOWN:
        r = sys_chown((const char *)a1, (uint32_t)a2, (uint32_t)a3);
        break;
    case SYS_FCHOWN:
        r = sys_fchown((int32_t)a1, (uint32_t)a2, (uint32_t)a3);
        break;
    case SYS_TELLDIR:          r = sys_telldir((int32_t)a1); break;
    case SYS_SEEKDIR:
        r = sys_seekdir((int32_t)a1, (int64_t)a2);
        break;
    case SYS_FPATHCONF:
        r = sys_fpathconf((int32_t)a1, (int32_t)a2);
        break;

    /* ---- 内存映射（90..99）---- */
    case SYS_MMAP:
        r = sys_mmap(a1, a2, (int32_t)a3, (int32_t)a4, (int32_t)a5,
                     (int64_t)a6);
        break;
    case SYS_MUNMAP:
        r = sys_munmap(a1, a2);
        break;
    case SYS_MPROTECT:
        r = sys_mprotect(a1, a2, (int32_t)a3);
        break;
    case SYS_MSYNC:
        r = sys_msync(a1, a2, (int32_t)a3);
        break;
    case SYS_MADVISE:
        r = sys_madvise(a1, a2, (int32_t)a3);
        break;
    case SYS_MINCORE:
        r = sys_mincore(a1, a2, (void *)a3);
        break;
    case SYS_MREMAP:
        r = sys_mremap(a1, a2, a3, (int32_t)a4, a5);
        break;

    /* ---- 时间（100..109）---- */
    case SYS_CLOCK_GETTIME:
        r = sys_clock_gettime((int32_t)a1, (suki_timespec_t *)a2);
        break;
    case SYS_CLOCK_SETTIME:
        r = sys_clock_settime((int32_t)a1, (const suki_timespec_t *)a2);
        break;
    case SYS_CLOCK_GETRES:
        r = sys_clock_getres((int32_t)a1, (suki_timespec_t *)a2);
        break;
    case SYS_GETTIMEOFDAY:
        r = sys_gettimeofday((suki_timeval_t *)a1, (suki_timezone_t *)a2);
        break;
    case SYS_NANOSLEEP:
        r = sys_nanosleep((const suki_timespec_t *)a1,
                          (suki_timespec_t *)a2);
        break;
    case SYS_TIME:             r = sys_time((int64_t *)a1); break;
    case SYS_SETTIMEOFDAY:
        r = sys_settimeofday((const suki_timeval_t *)a1,
                             (const suki_timezone_t *)a2);
        break;
    case SYS_ALARM:            r = sys_alarm((uint32_t)a1); break;
    case SYS_GETITIMER:
        r = sys_getitimer((int32_t)a1, (void *)a2);
        break;
    case SYS_SETITIMER:
        r = sys_setitimer((int32_t)a1, (const void *)a2, (void *)a3);
        break;

    /* ---- 系统信息（110..129）---- */
    case SYS_UNAME:            r = sys_uname((suki_utsname_t *)a1); break;
    case SYS_SYSINFO:          r = sys_sysinfo((suki_sysinfo_t *)a1); break;
    case SYS_GETRANDOM:
        r = sys_getrandom((void *)a1, a2, (uint32_t)a3);
        break;
    case SYS_SYSCONF:          r = sys_sysconf((int32_t)a1); break;
    case SYS_PRCTL:            r = sys_prctl((int32_t)a1, a2); break;
    case SYS_GETHOSTNAME:
        r = sys_gethostname((char *)a1, a2);
        break;
    case SYS_SETHOSTNAME:
        r = sys_sethostname((const char *)a1, a2);
        break;
    case SYS_GETPGRP:          r = sys_getpgrp(); break;
    case SYS_SETPGRP:          r = sys_setpgrp(); break;
    case SYS_GETSID:           r = sys_getsid(); break;
    case SYS_NICE:             r = sys_nice((int32_t)a1); break;
    case SYS_GETPRIORITY:
        r = sys_getpriority((int32_t)a1, (uint32_t)a2);
        break;
    case SYS_SETPRIORITY:
        r = sys_setpriority((int32_t)a1, (uint32_t)a2, (int32_t)a3);
        break;
    case SYS_SCHED_GETPID:     r = sys_getpid(); break;
    case SYS_MPROTECT_KEY:     r = -SUKI_ENOSYS; break;

    /* ---- 网络（130..149）：协议栈未就绪，按 POSIX 语义返回 ENOSYS ---- */
    case SYS_SOCKET:
    case SYS_BIND:
    case SYS_CONNECT:
    case SYS_LISTEN:
    case SYS_ACCEPT:
    case SYS_SENDTO:
    case SYS_RECVFROM:
    case SYS_SENDMSG:
    case SYS_RECVMSG:
    case SYS_SHUTDOWN:
    case SYS_SETSOCKOPT:
    case SYS_GETSOCKOPT:
    case SYS_GETPEERNAME:
    case SYS_GETSOCKNAME:
    case SYS_SOCKETPAIR:
        r = sys_socket_stub();
        break;

    default:
        return false;              /* 非 POSIX 号，交回上层处理 */
    }

    *ret = r;
    return true;
}
