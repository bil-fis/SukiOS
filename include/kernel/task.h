/*
 * include/kernel/task.h
 * -----------------------------------------------------------------------------
 * 任务控制块 (PCB)（手册 4.1）与调度接口。
 */
#ifndef _SUKI_KERNEL_TASK_H
#define _SUKI_KERNEL_TASK_H

#include <kernel/types.h>

struct kernel_port;   /* 前向声明（ipc/port.h） */
struct ool_map_node;   /* 前向声明（ipc/port.h，OOL 映射链表） */

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

#endif /* _SUKI_KERNEL_TASK_H */
