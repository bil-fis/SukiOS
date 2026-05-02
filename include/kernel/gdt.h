/* =============================================================================
 * SukiOS - 64 位 GDT（全局描述符表）
 *
 * 参考：OSDev GDT Tutorial, AMD64 APM Vol.2 4.8
 *
 * GDT 布局（64 位模式）：
 *   [0x00] 空描述符
 *   [0x08] 内核代码段（Long Mode, DPL 0）
 *   [0x10] 内核数据段（DPL 0）
 *   [0x18] 用户代码段（Long Mode, DPL 3）
 *   [0x20] 用户数据段（DPL 3）
 *   [0x28] TSS（64 位系统段，占 2 个 8 字节条目）
 * ============================================================================= */

#ifndef SUKIOS_GDT_H
#define SUKIOS_GDT_H

#include <stdint.h>

#define GDT_ENTRIES 7

/* IST 索引（与 IDT 门描述符的 IST 字段配合使用）
 * 参考：Intel SDM Vol.3 6.14.1, OSDev Task State Segment */
#define IST_DF      1   /* Double Fault（向量 #DF） */
#define IST_NMI     2   /* NMI（向量 2） — 可在任何时刻中断内核 */
#define IST_MC      3   /* Machine Check（向量 #MC） — 可在任何时刻中断内核 */

/* 64 位 TSS 结构体（AMD64 APM Vol.2 12.5.1, Intel SDM Vol.3 Figure 7-11）
 * 共 104 字节，用于 IST（中断栈表）和特权级切换时的栈指针 */
struct tss_entry {
    uint32_t reserved0;
    uint64_t rsp0;           /* Ring 0 栈指针（ring3→ring0 中断时 CPU 自动加载） */
    uint64_t rsp1;           /* Ring 1 栈指针 */
    uint64_t rsp2;           /* Ring 2 栈指针 */
    uint64_t reserved1;
    uint64_t ist1;           /* Interrupt Stack Table 1 */
    uint64_t ist2;           /* Interrupt Stack Table 2 */
    uint64_t ist3;           /* Interrupt Stack Table 3 */
    uint64_t ist4;           /* Interrupt Stack Table 4 */
    uint64_t ist5;           /* Interrupt Stack Table 5 */
    uint64_t ist6;           /* Interrupt Stack Table 6 */
    uint64_t ist7;           /* Interrupt Stack Table 7 */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;    /* I/O 权限位图偏移（>= TSS limit 表示无位图，禁止 ring3 I/O） */
} __attribute__((packed));

extern struct tss_entry kernel_tss;

/* GDT 选择子 */
#define GDT_SELECTOR_NULL       0x00
#define GDT_SELECTOR_KERNEL_CS  0x08
#define GDT_SELECTOR_KERNEL_DS  0x10
#define GDT_SELECTOR_USER_CS    0x18
#define GDT_SELECTOR_USER_DS    0x20
#define GDT_SELECTOR_TSS        0x28

void gdt_init(void);

#endif /* SUKIOS_GDT_H */
