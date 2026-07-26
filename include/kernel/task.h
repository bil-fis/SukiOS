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
    bool     is_user;               /* 是否 Ring3 进程 */
    bool     alive;                 /* 是否在就绪环中 */
    char     name[32];

    /* FPU/SSE 状态（D2 项：上下文切换时保存/恢复，512B 须 16 字节对齐） */
    uint8_t  fpu_state[512] __attribute__((aligned(16)));
    bool     fpu_valid;             /* 是否已保存过有效 FPU 状态 */

    bool     dead;                  /* 已退出，等待回收（B1 项） */
    struct task *wait_next;         /* 端口等待队列的下一个等待者（A4 项） */
    struct task *dead_next;         /* 全局“待回收”死亡链表链接（B1 项） */
    void    *ool_maps;              /* 本任务持有的 OOL 映射链表，退出时清理（A3 项） */

    struct task *next;              /* 就绪队列（循环链表） */
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
task_t *sched_current(void);
void   schedule(void);              /* 主动触发一次调度 */
void   task_yield(void);            /* 主动让出 CPU（sys_yield 底层） */
__attribute__((noreturn)) void task_exit_current(void);

uint64_t sched_next_pid(void);

#endif /* _SUKI_KERNEL_TASK_H */
