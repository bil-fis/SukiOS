/*
 * include/kernel/percpu.h
 * -----------------------------------------------------------------------------
 * per-CPU 数据体系（P0-3）。
 *
 * 模型：每个逻辑 CPU 一个 percpu_t 槽（静态数组 g_percpu[MAX_CPUS]），
 * 该 CPU 的 GS_BASE MSR (0xC0000101) 指向自己的槽。内核态代码用
 * `movl %gs:0, %reg` 一条指令即可取得本 CPU 索引，无锁、无查表。
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

#define MAX_CPUS 8

typedef struct percpu {
    uint32_t cpu_index;             /* gs:0  本 CPU 逻辑索引（0=BSP） */
    uint32_t lapic_id;              /* gs:4  本 CPU 的 LAPIC ID */
    uint64_t kstack_top;            /* gs:8  AP 空闲栈顶（诊断用） */
    volatile uint32_t online;       /* gs:16 AP 启动握手标志（1=在线） */
    uint32_t _pad;
    uint64_t ticks;                 /* 本 CPU 已处理的定时器/IPI 计数 */
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
