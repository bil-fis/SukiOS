/*
 * include/kernel/task.h
 * -----------------------------------------------------------------------------
 * 任务控制块 (PCB)（手册 4.1）与调度接口。
 */
#ifndef _SUKI_KERNEL_TASK_H
#define _SUKI_KERNEL_TASK_H

#include <kernel/types.h>
#define SUKI_KERNEL_BUILD 1
#include <sukios/posix.h>   /* SUKI_FD_MAX 等 POSIX ABI 常量 */

struct kernel_port;   /* 前向声明（ipc/port.h） */
struct ool_map_node;   /* 前向声明（ipc/port.h，OOL 映射链表） */
#include <kernel/suki_native.h>   /* SukiNative 对象/句柄类型（详见 kernel/suki_native.c） */
#include <kernel/elf.h>            /* 动态链接模块表 elf_module_t（task_t->modules） */

enum task_state { READY, RUNNING, BLOCKED, WAITING };

typedef struct task {
    uint64_t id;                    /* 全局唯一 PID */
    enum task_state state;

    /* 上下文切换 */
    uint64_t rsp;                   /* 内核栈指针（parked 于 context_switch 内） */
    uint64_t rip;                   /* 保留字段（手册 ABI） */
    uint64_t cr3;                   /* 进程页表基址（物理） */

    /* 调度参数 */
    uint64_t ticks_remaining;       /* 当前时间片剩余 */
    uint64_t priority;              /* 0-255，数值越小优先级越高 */

    /* IPC 关联 */
    struct kernel_port *reply_port; /* 当前正在等待的回复端口 */

    /* 文件系统上下文 */
    uint32_t cwd_cluster;           /* 当前工作目录 FAT32 簇号 */

    /* --- 实现附加字段 --- */
    uint64_t kstack_base;           /* 内核栈基址（用于释放） */
    uint64_t kstack_top;            /* 内核栈顶（TSS.rsp0 使用） */
    uint64_t user_rip;              /* Ring3 入口（用户虚拟地址） */
    uint64_t user_stack_top;        /* Ring3 栈顶（用户虚拟地址） */
    /* syscall 返回帧用的“暂存” RIP/RSP：进入 syscall 时保存用户 RIP/RSP，
     * execve 改写它们以跳转到新程序。必须是“每任务”的——若用全局变量，
     * 任务阻塞于 recv 期间另一任务执行 syscall 会覆盖该全局，导致原任务
     * 返回时 RSP 被破坏（实测表现为用户态缺页崩溃）。 */
    uint64_t scr_rip;               /* syscall 返回用 RIP（= 用户 RIP / execve 新入口） */
    uint64_t scr_rsp;               /* syscall 返回用 RSP（= 用户 RSP / execve 新栈顶） */
    bool     is_user;               /* 是否 Ring3 进程 */
    bool     alive;                 /* 是否在就绪环中 */
    bool     is_idle;               /* 是否为某 CPU 的 idle 任务（P0-R1） */
    uint32_t cpu;                   /* 绑定运行的 CPU（per-CPU 运行队列，P0-R1） */
    char     name[32];

    /* FPU/SSE 状态（D2 项：上下文切换时保存/恢复，512B 须 16 字节对齐） */
    uint8_t  fpu_state[512] __attribute__((aligned(16)));
    bool     fpu_valid;             /* 是否已保存过有效 FPU 状态 */

    bool     dead;                  /* 已退出，等待回收（B1 项） */
    struct task *wait_next;         /* 端口等待队列的下一个等待者（A4 项） */
    struct task *dead_next;         /* 全局“待回收”死亡链表链接（B1 项） */
    void    *ool_maps;              /* 本任务持有的 OOL 映射链表，退出时清理（A3 项） */
    void    *vma_list;              /* P0-5：VMA 链表头（vm_area_t*，按地址升序），
                                       mmap/栈增长区登记于此，#PF 按需补页查询 */

    /* --- 进程关系与退出同步（spawn/wait，类 Unix fork+exec+wait） --- */
    uint64_t parent_id;             /* 父任务 PID；idle=0。用 PID 而非指针，
                                       避免父退出后悬空指针被误用（sys_wait 仅比较值） */
    struct task *waiters;           /* 阻塞在本任务“退出”上的等待者链表头 */
    struct task *wait_link;         /* 等待者链表指针（链入父/子的 waiters） */
    uint64_t exit_code;             /* 退出码（task_exit_current 写入，供父 sys_wait 读） */
    bool     zombie;                /* 已退出、尚未被父 sys_wait 回收 */
    uint64_t wait_result;           /* 等待者被唤醒后读取的退出码（子退出时写入） */
    struct task *all_next;          /* 全局任务链表（g_all_tasks），供 pid 查找 */

    struct task *next;              /* 就绪队列（循环链表） */
    bool     in_rq;                  /* 是否已在某 CPU 运行队列中（sched_wake 判断是否需重新入队） */

    /* --- POSIX 进程属性（完整系统调用层，见 kernel/syscall/sys_posix.c） --- */
    int      fds[SUKI_FD_MAX];       /* 文件描述符表：值为全局 fd 槽号，-1 = 空 */
    uint64_t brk;                    /* 程序断点（堆顶，用户虚拟地址）；
                                      * 0 表示尚未初始化，首次 sys_brk 时定位
                                      * 到 ELF 镜像末尾之后并页对齐 */
    uint64_t brk_start;              /* 堆起始（= brk 初值，brk 不得小于它） */
    uint32_t uid;                    /* 实际用户 ID */
    uint32_t gid;                    /* 实际组 ID */
    uint32_t euid;                   /* 有效用户 ID */
    uint32_t egid;                   /* 有效组 ID */
    uint32_t umask;                  /* 文件创建掩码（sys_umask） */
    char     cwd[256];               /* 当前工作目录（绝对路径，"/" 表示根） */
    uint64_t utime_ticks;            /* 累计用户态节拍（times/rusage 用） */
    uint64_t stime_ticks;            /* 累计内核态节拍 */

    /* --- POSIX 等待与信号语义 --- */
    bool     wait_any;               /* 以 waitpid(-1,...) 方式阻塞等待【任意】
                                     * 子进程退出（父阻塞在此标志上，任何子退出
                                     * 时由 task_exit_current 唤醒它） */
    /* 注：信号投递已统一为 pending 位 + 返回路径 sig_deliver_check
     * （见 kernel/syscall/signal.c），不再使用 pending_kill/pending_signo 中间态。 */

    /* --- 线程（pthread）支持 --- */
    uint64_t tid;              /* 线程 id（gettid 返回） */
    uint64_t tgid;             /* 线程组 id（getpid 返回；fork=自身 id，clone=共享组长 tgid） */
    uint64_t fs_base;          /* 每线程 TLS 基址（FS base MSR；arch_prctl(SET_FS) 设置） */
    uint64_t clear_child_tid;  /* set_tid_address / CLONE_CHILD_CLEARTID：线程退出时清零并 futex_wake 的地址 */
    uint8_t  owns_as;          /* 1=独占地址空间，退出时负责销毁 cr3/vma；0=共享（线程，由最后退出者销毁） */

    /* --- 信号（signal/sigaction）支持 --- */
    struct suki_sigaction sigactions[SUKI_NSIG]; /* 每信号处置（handler/flags/mask） */
    suki_sigset_t sigmask;      /* 当前阻塞的信号集 */
    suki_sigset_t pending;      /* 待投递（pending）的信号集 */
    suki_sigset_t saved_sigmask;/* 进入处理器前的 sigmask，sigreturn 时还原 */
    uint8_t  in_signal;         /* 嵌套信号层数（>0 表示正在处理器中） */
    uint64_t sig_altstack;      /* 信号栈基址（sigaltstack；0=未设置） */
    uint64_t sig_altstack_size; /* 信号栈大小 */

    /* --- SukiNative 对象/句柄（Phase 1，详见 kernel/suki_native.c）---
     * 句柄表惰性分配（首次 SukiNative 调用时），fork/clone 子任务继承空表（不继承父句柄）。
     * 等待态：任务阻塞于 SYS_SUKI_WAIT 时，用 suki_wait_nodes[] 串入各对象的 waiter
     * 链表（每对象一个节点，避免单 next 指针无法同时挂多链）。 */
    suki_handle_entry_t *suki_handles;   /* NULL=尚未建表 */
    uint32_t            suki_handle_cap;
    suki_waitnode_t     suki_wait_nodes[SUKI_MAX_WAIT];
    int                 suki_wait_n;
    suki_object_t      *suki_wait_set[SUKI_MAX_WAIT];
    bool                suki_wait_active;
    suki_object_t      *suki_exit_notify;  /* 子任务退出通知链：本任务作为被监控的子进程时，
                                            * 所有「监控本任务的 PROC 对象」挂在此链上；
                                            * suki_proc_notify_exit 遍历并脱离。仅 PROC 对象用。 */

    /* --- 动态链接模块表（elf.c 维护）---
     * modules[0] 恒为主程序；modules[1..nmodules-1] 为 DT_NEEDED 共享库或 dlopen 加载的 .sl。
     * elf_link_dynamic 在 execve/spawn 时填充；elf_dlopen 运行时追加。img 在内核侧持有
     * ELF 副本用于符号解析，进程退出时由 kernel 释放。 */
    elf_module_t modules[ELF_MODULE_MAX];
    int          nmodules;          /* 0=纯静态未链接；>=1 含主程序 */
} task_t;

/* FPU/SSE 状态保存与恢复原语（实现见 sched/switch.S） */
void fpu_fxsave(void *buf);
void fpu_fxrstor(void *buf);
void fpu_fninit(void);
void fpu_init(void);

#define TIME_SLICE_TICKS  2         /* 每个时间片 = 2 个节拍（20ms @100Hz） */

void   sched_init(void);
task_t *task_create_kernel(void (*entry)(void *), void *arg, const char *name);

/* 创建 Ring3 任务：把 [blob, blob+size) 拷入新地址空间 user_rip 处执行 */
task_t *task_create_user(const void *blob, size_t size, const char *name);
/* 同上，但装载时构造含 argc/argv/envp 的初始栈（spawn 子进程用） */
task_t *task_create_user_args(const void *blob, size_t size,
                              int argc, const char *const argv[],
                              int envc, const char *const envp[],
                              const char *name);
task_t *sched_current(void);
void   schedule(void);              /* 主动触发一次调度 */

/* 唤醒一个阻塞任务（IPC/等待协议用）：置 READY，若已被 schedule 移出运行
 * 队列则重新入队到其绑定 CPU，并向其所在核发 RESCHED IPI。
 * 必须在关中断/持适当锁的上下文调用；本函数内部持 g_sched_lock（irqsave），
 * 故不会在持锁期被 IPI 嵌套（interrupt gate 自动 CLI）。 */
void   sched_wake(task_t *t);
void   sched_set_fs_base(uint64_t base);   /* 设置当前 CPU 的 FS base（每线程 TLS 基址） */
task_t *sched_create_idle(uint32_t cpu);  /* 为某 CPU 建 idle 任务（P0-R1） */

/* sys_wait 核心：等待子任务退出（SMP 安全，g_sched_lock 保护，P0-R1）。
 * 返回 0 成功（*rc_out=退出码）；-1 pid 无效或非当前任务子进程。 */
int64_t task_wait_child(uint64_t child_pid, uint64_t *rc_out);
void   task_yield(void);            /* 主动让出 CPU（sys_yield 底层） */
__attribute__((noreturn)) void task_exit_current(uint64_t code);

/* 按 PID 在全局任务表中查找任务（含尚未被回收的 zombie）；找不到返回 NULL */
task_t *task_lookup(uint64_t pid);

/* 立即回收一个已退出任务（从死亡链表与全局表摘除并释放）；供 sys_wait 使用 */
void task_reap(task_t *t);

uint64_t sched_next_pid(void);

/* ========================================================================== */
/*  POSIX 进程层：fork / execve / 资源继承                                     */
/* ========================================================================== */

/*
 * 完整 fork()：以当前任务为模板创建子进程。
 *   - 新 task（新 PID、parent_id = 父 PID）、独立内核栈（守卫页）；
 *   - 地址空间：vmm_create_address_space + vmm_fork_cow（写时复制共享），
 *     VMA 链表深拷贝（vma_clone_all）；
 *   - fd 表：fd_fork_clone（共享槽，POSIX 共享文件偏移语义）；
 *   - 用户寄存器：复制父的 syscall GPR 帧（g_syscall_gpr[cpu]），合成内核
 *     栈帧令子进程经 context_switch -> fork_child_return 以 rax=0 返回用户态。
 * 返回子进程 task 指针，失败返回 NULL。
 * 实现见 kernel/syscall/sys_posix.c。
 */
task_t *task_fork(void);

/* 在当前任务上下文中装载并执行新 ELF（execve 语义）。
 * path 为内核缓冲中的路径；argv/envp 为内核缓冲中的字符串数组（已校验）。
 * 成功不返回（装入新镜像）；失败返回负 errno。 */
int64_t task_execve(const char *path, int argc, const char *const argv[],
                    int envc, const char *const envp[]);

/* 记账：在时钟节拍中给当前任务累加 utime/stime（times/rusage/clock_gettime
 * 的 PROCESS_CPUTIME_ID 数据源）。user_mode=true 记用户态，否则记内核态。 */
void task_account_tick(bool user_mode);

/* 发布一个「已由调用方完整构造好」的任务（fork 用）：与 task_create_kernel
 * 的发布路径一致（入全局表 + 入运行队列 + RESCHED IPI），但不重新构造跳板
 * 内核栈帧——fork 的子进程帧由 sys_fork 自行合成。 */
void task_publish_ready(task_t *t);

/*
 * POSIX waitpid 核心（SMP 安全，g_sched_lock 保护）。
 *   pid   >0 等待指定子进程；-1 等待任意子进程；其它值返回 -EINVAL
 *   nohang  true = WNOHANG（无已退出子则立即返回 -EAGAIN，绝不阻塞）
 * 返回 0 成功（*pid_out=退出子 PID，*rc_out=退出码）；<0 为负 errno
 * （-ECHILD 无符合子进程 / -EAGAIN WNOHANG 且无就绪 / -EINVAL 参数非法）。
 */
int64_t task_wait_any_child(int64_t pid, uint64_t *pid_out,
                            uint64_t *rc_out, bool nohang);

/* 当前存活任务总数（含 idle 与 zombie），供 sys_sysinfo 的 procs 字段使用。
 * 计数器是调度器私有状态，经本函数导出，避免外部直接引用 static 变量。 */
uint32_t task_count(void);

/*
 * POSIX kill：向 pid 投递信号。
 * 语义：置目标的 pending 位（新信号框架），由目标在【syscall 返回用户态的边界】
 * 经 sig_deliver_check 按处置（handler / 默认终止 / 忽略）处理——绝不就地杀死
 * 正在运行/持锁的任务（与 Linux 的 TIF_SIGPENDING 处理时机一致）；若目标正
 * 阻塞在等待队列，则同时唤醒它。返回 0 成功；<0 为负 errno。
 */
int task_signal(uint64_t pid, int signo);

/* 向任务 t 投递信号 sig：置 pending 位；若 t 不在运行态则唤醒，使其尽快返回
 * 用户态检查 pending 信号。由 sig_deliver_check 在 syscall 返回边界完成真实投递
 * 或默认动作。定义见 kernel/syscall/signal.c。 */
void task_signal_send(task_t *t, int sig);

#endif /* _SUKI_KERNEL_TASK_H */
