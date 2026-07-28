/*
 * include/kernel/kaslr.h
 * -----------------------------------------------------------------------------
 * P0-8 / P0-R1 内核代码段 KASLR（高半区基址随机滑动）。
 *
 * 引导期 boot.S 把整个内核高半区基址随机滑动 g_kernel_slide（1GB 对齐），
 * 并通过 R_X86_64_64 重定位让全部绝对地址引用随滑动量平移。运行时 C 代码
 * 通过 PHYS_TO_VIRT()（其底层 KERNEL_BASE 已在 boot 期被重定位）透明获得
 * 随机后的基址，无需再手动加 g_kernel_slide。本头仅声明供需要“原始滑动量”
 * 的诊断/报告路径使用。
 */
#ifndef _SUKI_KERNEL_KASLR_H
#define _SUKI_KERNEL_KASLR_H

#include <kernel/types.h>

/* 引导期由 boot.S 写入：高半区基址随机滑动量（字节，1GB 对齐）。
 * 位于 .bss，boot.S 在其切到新页表前写旧虚拟地址（物理同体），重载 CR3
 * 后于新虚拟地址读取到同一物理，值保持一致。 */
extern uint64_t g_kernel_slide;

#endif /* _SUKI_KERNEL_KASLR_H */
