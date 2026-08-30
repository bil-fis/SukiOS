/*
 * include/kernel/smp.h
 * -----------------------------------------------------------------------------
 * SMP：AP 启动、IPI（处理器间中断）、TLB shootdown（P0-3）。
 *
 * 对称多核模型（已实现并验证）：
 *   - 全部 AP 经 INIT-SIPI-SIPI 唤醒进入 64 位长模式，安装各自的
 *     GDT/TSS/IDT/LAPIC/percpu，并在各自 CPU 上使能安全控制位
 *     (CR4.SMAP/SMEP/UMIP + EFER.NXE，见 security.c cpu_apply_security_features)，
 *     随后启动 LAPIC 周期定时器(100Hz, IRQ0)→sched_tick→schedule，
 *     因此 AP 也参与 RR 调度、可运行 Ring3 用户任务；
 *   - 调度器为对称 SMP RR（per-CPU runqueue + 全局 g_sched_lock），
 *     当某 CPU 运行队列无可运行任务时通过 work-stealing 从其它 CPU
 *     窃取就绪任务（见 sched.c steal_task），实现负载均衡；
 *   - 任务创建时按 smp_online_count() 静态 RR 绑定初始 CPU，后续由窃取迁移。
 *
 * 【编译选项：SMP 为可选特性，默认关闭】
 *   CONFIG_SMP=0（默认，单核构建）：smp_init() 只注册 IPI handler 并报告
 *   单核形态，不探测/唤醒任何 AP；smp_online_count() 恒为 1；TLB shootdown
 *   与 halt-others 退化为空操作。AP 跳板(ap_boot.S)不编入镜像，MAX_CPUS=1。
 *   CONFIG_SMP=1（make SMP=1）：上述对称多核能力全部启用，MAX_CPUS=8。
 *   详见 include/kernel/config.h。
 *
 * IPI 向量分配（避开 0x20..0x2F 的 IRQ 与 0xFF 伪中断）：
 *   0xF0 IPI_RESCHED   —— 重调度请求（唤醒目标 CPU 的 idle 循环或触发再调度；
 *                          亦用于发布/唤醒任务时通知可能的空闲 AP 来窃取工作）
 *   0xF1 IPI_TLB_FLUSH —— TLB shootdown：收到后整体重载 CR3
 *   0xF2 IPI_HALT      —— panic 停机：收到后 cli+hlt 永久停车
 */
#ifndef _SUKI_KERNEL_SMP_H
#define _SUKI_KERNEL_SMP_H

#include <kernel/types.h>

#define IPI_RESCHED    0xF0
#define IPI_TLB_FLUSH  0xF1
#define IPI_HALT       0xF2
/* P0-R3：gdbstub 会话冻结——收到后其它 CPU 在中断上下文自旋，直到
 * gdbstub 释放（continue/step/detach），保证调试快照期间内存/寄存器不漂移 */
#define IPI_GDB_FREEZE 0xF3

/* 依据 ACPI MADT 枚举启动全部 AP。须在 acpi/lapic/clock（TSC 校准，供
 * 延时用）就绪后调用。返回在线 CPU 总数（含 BSP）。 */
uint32_t smp_init(void);

/* 当前在线 CPU 数（含 BSP；未做 SMP 初始化时为 1） */
uint32_t smp_online_count(void);

/* 向所有其它 CPU 广播 TLB 刷新 IPI（页表项收回后调用）。
 * 单核或 AP 未启动时为空操作。 */
void smp_tlb_shootdown(void);

/* panic 路径：命令所有其它 CPU 立即 cli+hlt（防止半死状态继续跑坏数据）。 */
void smp_halt_others(void);

#endif /* _SUKI_KERNEL_SMP_H */
