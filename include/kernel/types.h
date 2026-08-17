/*
 * include/kernel/types.h
 * -----------------------------------------------------------------------------
 * SukiOS 基础类型定义。-ffreestanding 环境下可安全使用编译器自带的
 * <stdint.h> / <stddef.h> / <stdbool.h>（它们不依赖 libc）。
 */
#ifndef _SUKI_KERNEL_TYPES_H
#define _SUKI_KERNEL_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* 内存布局红线（手册第 2 章） */
#define KERNEL_BASE   0xFFFF800000000000UL
#define KERNEL_PHYS   0x0000000000100000UL   /* 内核物理加载地址 1MB */
#define PAGE_SIZE     4096UL

/* P0-8 KASLR：运行期高半区实际基址 = KERNEL_BASE + g_kernel_slide。
 * boot.S 在切页表前写入（见 boot/boot.S KASLR 块）；定义在 kernel/mm/vmm.c。
 * PHYS_TO_VIRT/VIRT_TO_PHYS 必须使用运行期变量而非编译期常量——否则基址
 * 滑动后所有物理直映访问全部落在未映射的旧窗口（编译期字面量不产生
 * R_X86_64_64 重定位项，boot 期无从修补）。-mcmodel=large 下对 g_virt_base
 * 的取址是 movabsq 绝对地址，本身会被 KASLR 重定位循环修补，自洽。 */
extern uint64_t g_virt_base;

/* 物理地址 <-> 高半区虚拟地址（恒等偏移映射区，基址运行期随机化） */
#define PHYS_TO_VIRT(p)  ((void *)((uint64_t)(p) + g_virt_base))
#define VIRT_TO_PHYS(v)  ((uint64_t)(v) - g_virt_base)

/* 段选择子（手册 5.1） */
#define KERNEL_CS   0x08
#define KERNEL_DS   0x10
#define USER_CS     0x1B    /* RPL = 3 */
#define USER_DS     0x23    /* RPL = 3 */

#endif /* _SUKI_KERNEL_TYPES_H */
