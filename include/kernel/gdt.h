/*
 * include/kernel/gdt.h
 * -----------------------------------------------------------------------------
 * 全局描述符表 (GDT) 与任务状态段 (TSS)。
 *
 * 段布局（严格遵循手册 5.1 的选择子）：
 *   0x00 空描述符
 *   0x08 内核代码段 (64 位, DPL0)    KERNEL_CS
 *   0x10 内核数据段        (DPL0)    KERNEL_DS
 *   0x18 用户代码段 (64 位, DPL3)    USER_CS = 0x1B (RPL3)
 *   0x20 用户数据段        (DPL3)    USER_DS = 0x23 (RPL3)
 *   0x28 TSS 描述符（占用两个 8 字节槽）
 *
 * 说明：进入用户态与 syscall 返回统一采用 IRETQ，因此选择子无需满足
 *       SYSRET 的 +8/+16 固定偏移约束，可精确匹配手册选择子。
 *       SYSCALL 入口要求内核 CS=0x08、内核 SS=0x10（+8），本布局满足。
 */
#ifndef _SUKI_KERNEL_GDT_H
#define _SUKI_KERNEL_GDT_H

#include <kernel/types.h>

#define GDT_TSS_SELECTOR  0x28

void gdt_init(void);
/* AP 专用：安装该 CPU 自己的 GDT+TSS（TR 每 CPU 独立，P0-3）。cpu>=1。 */
void gdt_init_ap(uint32_t cpu);
/* 设置发生特权级切换（Ring3->Ring0）时使用的内核栈指针 */
void tss_set_rsp0(uint64_t rsp0);

/* P0-8：更新 BSP TSS 的 IST 槽（n=1..7），用于安装守卫页 IST 栈 */
void tss_set_ist(int n, uint64_t stack_top);

#endif /* _SUKI_KERNEL_GDT_H */
