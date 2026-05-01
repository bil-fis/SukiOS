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

/* 64 位 TSS 结构体（AMD64 APM Vol.2 12.5.1）
 * 用于 IST（中断栈表）和栈切换 */
struct tss_entry {
    uint32_t reserved0;
    uint64_t rsp0;           /* Ring 0 栈指针 */
    uint64_t rsp1;           /* Ring 1 栈指针 */
    uint64_t rsp2;           /* Ring 2 栈指针 */
    uint64_t reserved1;
    uint64_t ist1;           /* Interrupt Stack Table 1（用于 Double Fault） */
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;    /* I/O 权限位图偏移（0xFFFF 表示不映射 I/O 端口） */
    uint32_t reserved4;
    uint32_t reserved5;
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
