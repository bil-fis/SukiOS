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

/* 物理地址 <-> 高半区虚拟地址（恒等偏移映射区） */
#define PHYS_TO_VIRT(p)  ((void *)((uint64_t)(p) + KERNEL_BASE))
#define VIRT_TO_PHYS(v)  ((uint64_t)(v) - KERNEL_BASE)

/* 段选择子（手册 5.1） */
#define KERNEL_CS   0x08
#define KERNEL_DS   0x10
#define USER_CS     0x1B    /* RPL = 3 */
#define USER_DS     0x23    /* RPL = 3 */

#endif /* _SUKI_KERNEL_TYPES_H */
