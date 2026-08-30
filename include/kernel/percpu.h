/*
 * include/kernel/percpu.h
 * -----------------------------------------------------------------------------
 * per-CPU 数据体系（P0-3）。
 *
 * 模型：每个逻辑 CPU 一个 percpu_t 槽（静态数组 g_percpu[MAX_CPUS]），
 * 该 CPU 的 GS_BASE MSR (0xC0000101) 指向自己的槽。内核态代码用
 * `movl %gs:0, %reg` 一条指令即可取得本 CPU 索引，无锁、无查表。
 *
 * 编译选项：SMP 是【可选特性】，默认关闭（CONFIG_SMP=0，见 config.h）。
 * 单核构建下 MAX_CPUS=1，per-CPU 体系依然完整保留（调度器/IPC/syscall
 * 快速路径都以 cpu_local()->xxx 编写），只是永远只有 cpu0 一个槽——这样
 * 单核与多核共用同一套代码路径，切换编译选项时无需改动业务代码。
 *
 * 关键约束：
 *   - 本内核不使用 swapgs：用户态不会改写 GS 段（无 wrgsbase，CR4.FSGSBASE
 *     未开启；enter_user_mode 不重载 gs 选择子），因此 Ring0/Ring3 全程
 *     GS_BASE 稳定指向 percpu 槽，中断/异常路径无需 swapgs 配对。
 *   - 字段偏移固定（cpu_index 必须在 offset 0），汇编依赖。
 */
#ifndef _SUKI_KERNEL_PERCPU_H
#define _SUKI_KERNEL_PERCPU_H

#include <kernel/types.h>
#include <kernel/config.h>   /* CONFIG_SMP / CONFIG_SMP_MAX_CPUS（编译选项） */

/*
 * MAX_CPUS —— 逻辑 CPU 槽位上限。
 * 由编译选项 CONFIG_SMP 决定（见 include/kernel/config.h）：
 *   多核构建(CONFIG_SMP=1) = 8；单核构建(CONFIG_SMP=0，默认) = 1。
 * 单核下所有 per-CPU 数组（g_percpu[]、g_syscall_kstack[]、g_scratch[]，
 * 以及 gdt.c 的 AP GDT/TSS/IST 数组）自动收敛为单元素。
 */
#define MAX_CPUS CONFIG_SMP_MAX_CPUS

struct task;   /* 前向声明：percpu 含调度器状态，避免与 task.h 循环包含 */

typedef struct percpu {
    uint32_t cpu_index;             /* gs:0  本 CPU 逻辑索引（0=BSP） */
    uint32_t lapic_id;              /* gs:4  本 CPU 的 LAPIC ID */
    uint64_t kstack_top;            /* gs:8  AP 空闲栈顶（诊断用） */
    volatile uint32_t online;       /* gs:16 AP 启动握手标志（1=在线） */
    uint32_t _pad;
    uint64_t ticks;                 /* 本 CPU 已处理的定时器/IPI 计数 */

    /* ---- P0-R1 对称多核调度：per-CPU 调度状态 ---- */
    struct task *current_task;      /* 本 CPU 当前运行任务（替代全局 g_current） */
    struct task *idle_task;         /* 本 CPU idle 任务 */
    struct task *rq_head;           /* 本 CPU 运行队列头（单向链表，t->next 链接） */
    struct task *rq_tail;           /* 本 CPU 运行队列尾 */
    uint32_t    rq_count;           /* 本 CPU 运行队列任务数（含 idle） */
    volatile uint32_t in_idle;      /* 本 CPU 是否处于 hlt 空闲（供 IPI 唤醒参考） */
    uint64_t user_switches;         /* 本 CPU 切换到 Ring3 任务的次数（负载均衡观测） */
} percpu_t;

extern percpu_t g_percpu[MAX_CPUS];

/* 安装本 CPU 的 percpu 槽：填充字段并写 GS_BASE MSR。每个 CPU 上电后
 * （BSP 在 lapic_init 后、AP 在进入 ap_main 后）各自调用一次。 */
void percpu_install(uint32_t cpu_index, uint32_t lapic_id);

/* 取得当前 CPU 的逻辑索引。percpu 未安装完成前安全返回 0（BSP 早期）。 */
uint32_t cpu_index(void);

/* 当前 CPU 的 percpu 槽指针 */
percpu_t *cpu_local(void);

#endif /* _SUKI_KERNEL_PERCPU_H */
