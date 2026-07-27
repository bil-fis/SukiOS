/*
 * include/kernel/smp.h
 * -----------------------------------------------------------------------------
 * SMP：AP 启动、IPI（处理器间中断）、TLB shootdown（P0-3）。
 *
 * 当前阶段模型（诚实声明）：
 *   - 全部 AP 经 INIT-SIPI-SIPI 唤醒进入 64 位长模式，安装各自的
 *     GDT/TSS/IDT/LAPIC/percpu 后进入 idle(hlt) 循环，响应 IPI；
 *   - 任务调度仍由 BSP 独占（LAPIC 定时器仅 BSP 开启），把调度域扩展
 *     到 AP（per-CPU runqueue + swapgs 化 syscall 栈）随后续里程碑推进；
 *   - 因此本阶段锁体系保护的是「BSP 任务流 vs AP 的 IPI/诊断路径」，
 *     以及为多核调度提前铺设的正确性地基。
 *
 * IPI 向量分配（避开 0x20..0x2F 的 IRQ 与 0xFF 伪中断）：
 *   0xF0 IPI_RESCHED   —— 重调度请求（当前为骨架：AP 无 runqueue）
 *   0xF1 IPI_TLB_FLUSH —— TLB shootdown：收到后整体重载 CR3
 *   0xF2 IPI_HALT      —— panic 停机：收到后 cli+hlt 永久停车
 */
#ifndef _SUKI_KERNEL_SMP_H
#define _SUKI_KERNEL_SMP_H

#include <kernel/types.h>

#define IPI_RESCHED    0xF0
#define IPI_TLB_FLUSH  0xF1
#define IPI_HALT       0xF2

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
