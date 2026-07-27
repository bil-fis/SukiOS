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

/* 金丝雀熵重播种（实现于 stack_canary.c，-fno-stack-protector 编译） */
void stack_canary_reseed(void);

#endif /* _SUKI_KERNEL_SECURITY_H */
