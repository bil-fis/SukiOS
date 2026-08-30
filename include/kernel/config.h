/*
 * include/kernel/config.h
 * -----------------------------------------------------------------------------
 * SukiOS 内核【编译期配置】集中定义（Kconfig 风格的轻量实现）。
 *
 * 背景与动机：
 *   SukiOS 自 P0-3 起具备对称多核（SMP）能力（INIT-SIPI-SIPI 唤醒 AP、
 *   per-CPU 运行队列 + work-stealing、IPI/TLB shootdown、per-CPU GS 数据）。
 *   但项目当前阶段的开发、验证与调优主线是【单核】：单核路径更简单、
 *   可复现，且不受跨核竞态/丢失唤醒等非确定性因素干扰。因此把 SMP 做成
 *   显式编译选项，**默认关闭**，需要时以 `make SMP=1` 打开。
 *
 * 覆盖机制（两级，保证任何构建方式下语义确定）：
 *   1) 命令行/构建系统优先：Makefile 依据 SMP 变量生成 build/config.h，
 *      并以 `-include $(BUILD)/config.h` 注入到【每一个】内核编译单元的最
 *      前面（先于任何 #include 生效）。该头文件被 -MMD 记录进 .d 依赖，
 *      因此 SMP 开关一变，全部 .o 自动重编，绝不会出现「新旧配置混链」。
 *   2) 本文件的 #ifndef 兜底：不经 Makefile 直接编译（如手工 gcc 单文件
 *      检查、外部工具扫描源码）时，取本文件的默认值 = 单核。
 *
 * 受本配置影响的代码面（详见各自文件中的 #if CONFIG_SMP 分支）：
 *   - include/kernel/percpu.h ：MAX_CPUS = 8（多核） / 1（单核）
 *   - kernel/arch/x86_64/smp.c：AP 启动（跳板拷贝 + INIT-SIPI-SIPI）与 AP
 *                               idle、IPI 自检仅在 CONFIG_SMP=1 时编译
 *   - Makefile                ：CONFIG_SMP=0 时从源文件列表剔除 ap_boot.S；
 *                               QEMU -smp 默认值随配置联动（1 / 4）
 *   - kernel/kmain.c          ：启动日志明报告当前构建的单核/多核形态
 *
 * 注意：IPI 向量（0xF0 RESCHED / 0xF1 TLB / 0xF2 HALT / 0xF3 GDB 冻结）与
 * 其 handler 注册在【两种配置下都保留】——单核构建中 RESCHED 以 self-IPI
 * 形式承担「发布新任务/唤醒 waiter 时立即触发一次调度」的生存性职责
 * （见 sched.c task_publish / sched_wake 注释），不可裁剪。
 */
#ifndef _SUKI_KERNEL_CONFIG_H
#define _SUKI_KERNEL_CONFIG_H

/*
 * CONFIG_SMP —— 对称多核支持开关。
 *   0（默认）：单核构建。BSP 独占 per-CPU 槽 0，MAX_CPUS=1，无 AP 上线路径。
 *   1        ：多核构建。编译 AP 跳板与上线路径，MAX_CPUS=8，全量 SMP 特性。
 */
#ifndef CONFIG_SMP
#define CONFIG_SMP 0
#endif

/*
 * CONFIG_SMP_MAX_CPUS —— 逻辑 CPU 槽位上限（即 percpu.h 的 MAX_CPUS）。
 * 单核构建取 1：g_percpu[]、g_syscall_kstack[]、g_scratch[]、AP 的
 * g_gdt_ap[]/g_tss_ap[]/IST 栈等 per-CPU 数组全部收敛为单元素，既省内存
 * 也让「越界访问第二颗 CPU」在编译/链接层面无处可去。
 */
#if CONFIG_SMP
#define CONFIG_SMP_MAX_CPUS 8
#else
#define CONFIG_SMP_MAX_CPUS 1
#endif

#endif /* _SUKI_KERNEL_CONFIG_H */
