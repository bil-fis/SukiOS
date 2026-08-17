/*
 * include/kernel/security.h
 * -----------------------------------------------------------------------------
 * 内核安全地基（P0-8）：集中收口的加固初始化与状态报告。
 *
 * 覆盖项（与 step10_todos P0-8 对照）：
 *   - SMAP/SMEP：boot.S 已启用（CPUID 探测），此处纳入状态报告；
 *   - UMIP：禁止 Ring3 执行 sgdt/sidt/sldt/smsw/str（防内核地址泄露）；
 *   - 栈保护：-fstack-protector-strong 全内核插桩（既有）+ 金丝雀熵重播种
 *     （stack_canary_reseed，kmain 极早期调用）+ IST 守卫页栈（本模块）；
 *   - KASLR：内核堆基址随机滑移（kmalloc.c）+ 用户栈 ASLR（既有）；
 *     内核代码段 KASLR 需 PIE 内核+引导链重定位，随 P0-6 UEFI 路径交付；
 *   - KPTI：检测 IA32_ARCH_CAPABILITIES.RDCL_NO 判定 CPU 是否 Meltdown
 *     免疫，输出 KPTI 需求结论（免疫 CPU 上双页表纯损耗，不部署）。
 *
 * 调用关系：kmain -> stack_canary_reseed()（极早期）；
 *           kmain -> security_init()（vmm/kheap 就绪后）。
 */
#ifndef _SUKI_KERNEL_SECURITY_H
#define _SUKI_KERNEL_SECURITY_H

#include <kernel/types.h>

/* 安全加固总初始化：UMIP 使能、NXE 校验、Meltdown 免疫检测、
 * 守卫页 IST 栈安装（替换启动期静态 IST 栈）、汇总报告。 */
void security_init(void);

/* 在当前 CPU 上按 CPUID 探测结果应用安全控制位：CR4.SMEP / CR4.SMAP /
 * CR4.UMIP 与 EFER.NXE。BSP 由 security_init 调用；AP 在 ap_main 中调用，
 * 以保证每个逻辑核的用户内存保护位一致（单核假设下 AP 漏设会留下安全缺口）。 */
void cpu_apply_security_features(void);

/* 金丝雀熵重播种（实现于 stack_canary.c，-fno-stack-protector 编译） */
void stack_canary_reseed(void);

/* ---- P0-8 KPTI 守卫页 IST 栈布局（与 security.c 实现一致，供 vmm.c 在
 *      影子页表内映射这两条栈，使 NMI/#DF 从用户态进入时仍能压栈不 #PF）----
 *   窗口基址 0xFFFFD00000000000；每个 idx（0=#DF/IST1, 1=NMI/IST2）占
 *   (1 守卫页 + IST_STACK_PAGES 映射页 + 1 隔离页)，映射页 VA 从窗口基址
 *   +1 页起。vmm.c 的 vmm_shadow_populate 据此把页 1..IST_STACK_PAGES
 *   映射进影子（守卫页刻意不映射）。 */
#define IST_GUARD_BASE    0xFFFFD00000000000UL
#define IST_STACK_PAGES   4
#define IST_WINDOW_PAGES  (1 + IST_STACK_PAGES + 1)
#define IST_PAGE_SIZE     4096UL
#define IST_VA(idx, page) (IST_GUARD_BASE + (uint64_t)(idx) * \
                           IST_WINDOW_PAGES * IST_PAGE_SIZE + \
                           (uint64_t)(1 + (page)) * IST_PAGE_SIZE)

#endif /* _SUKI_KERNEL_SECURITY_H */
