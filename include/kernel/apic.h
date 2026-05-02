/* =============================================================================
 * SukiOS - APIC（高级可编程中断控制器）
 *
 * 参考：OSDev APIC, Intel SDM Vol.3 Chapter 10
 *
 * 包含 Local APIC 和 I/O APIC 的初始化。
 * MMIO 基地址通过 VMM 映射到内核专用 MMIO 区域。
 * ============================================================================= */

#ifndef SUKIOS_APIC_H
#define SUKIOS_APIC_H

#include <stdint.h>
#include <stdbool.h>

/* ---- MSR ---- */
#define IA32_APIC_BASE_MSR      0x1B
#define IA32_APIC_BASE_ENABLE   (1ULL << 11)
#define IA32_APIC_BASE_MASK     0xFFFFF000ULL

/* ---- 中断向量分配 ---- */
#define IRQ_BASE                0x20    /* 中断向量基址（避开 CPU 异常 0-31） */
#define IRQ_KEYBOARD            (IRQ_BASE + 1)   /* IRQ1: PS/2 键盘 */
#define IRQ_APIC_TIMER          (IRQ_BASE + 0)   /* 使用 IRQ0 向量作为 LAPIC 定时器 */

/* ---- Local APIC 寄存器偏移 ---- */
#define LAPIC_ID_REG            0x020   /* Local APIC ID */
#define LAPIC_VERSION_REG       0x030   /* 版本 */
#define LAPIC_EOI               0x0B0   /* End of Interrupt（只写） */
#define LAPIC_SVR               0x0F0   /* Spurious Interrupt Vector */
#define LAPIC_ICR_LOW           0x300   /* Interrupt Command Register [31:0] */
#define LAPIC_ICR_HIGH          0x310   /* Interrupt Command Register [63:32] */

/* ---- Local APIC Timer 寄存器 ---- */
#define LAPIC_LVT_TIMER         0x320   /* LVT Timer Register */
#define LAPIC_INIT_COUNT        0x380   /* Initial Count Register */
#define LAPIC_CUR_COUNT         0x390   /* Current Count Register */
#define LAPIC_DIV_CONF          0x3E0   /* Divide Configuration Register */

/* ---- LVT Timer 位 ---- */
#define LAPIC_LVT_MASK          (1U << 16)  /* 中断屏蔽 */
#define LAPIC_LVT_PERIODIC      (1U << 17)  /* 周期模式 */
#define LAPIC_LVT_ONE_SHOT      (0U << 17)  /* 单次触发模式 */

/* ---- Divide Configuration 值 ---- */
#define LAPIC_DIV_1             0x0B    /* 除以 1 */
#define LAPIC_DIV_16            0x03    /* 除以 16 */
#define LAPIC_DIV_128           0x00    /* 除以 128 */

/* ---- Local APIC SVR 位 ---- */
#define LAPIC_SVR_ENABLE        (1 << 8)  /* 软件启用 APIC */

/* ---- I/O APIC 寄存器 ---- */
#define IOAPIC_REGSEL           0x00    /* 寄存器选择 */
#define IOAPIC_REGWIN           0x10    /* 数据窗口 */
#define IOAPIC_ID_REG           0x00    /* IOAPIC ID 寄存器编号 */
#define IOAPIC_VER_REG          0x01    /* 版本寄存器编号 */
#define IOAPIC_REDIR_BASE       0x10    /* 重定向条目起始编号 */

/* ---- I/O APIC 重定向条目位 ---- */
#define IOAPIC_REDIR_MASK       (1U << 16)  /* 中断屏蔽 */
#define IOAPIC_REDIR_POLARITY   (1U << 13)  /* 极性：1=Active Low, 0=Active High */
#define IOAPIC_REDIR_TRIGGER    (1U << 15)  /* 触发模式：1=Level, 0=Edge */

/* ---- 伪中断向量 ---- */
#define APIC_SPURIOUS_VECTOR    0xFF

/* ---- 全局 APIC MMIO 基地址（由 apic_init 设置，通过 VMM 映射） ---- */
extern volatile uint32_t *apic_lapic_base;
extern volatile uint32_t *apic_ioapic_base;

/* ---- 函数声明 ---- */

/**
 * apic_init - 初始化 APIC 子系统
 *
 * 前置条件：VMM 已初始化（vmm_init()）
 *
 * 步骤：
 *   1. 解析 ACPI MADT 获取 APIC 地址
 *   2. 通过 MSR 启用 Local APIC
 *   3. 使用 VMM 永久映射 LAPIC/IOAPIC MMIO
 *   4. 配置 Spurious Interrupt Vector
 *   5. 初始化 I/O APIC（屏蔽所有重定向条目）
 */
void apic_init(void);

/* Local APIC 寄存器读写 */
void lapic_write(uint32_t offset, uint32_t value);
uint32_t lapic_read(uint32_t offset);

/* I/O APIC 寄存器读写 */
void ioapic_write(uint32_t reg, uint32_t value);
uint32_t ioapic_read(uint32_t reg);

/* I/O APIC 重定向条目配置
 * @irq_pin:    IOAPIC 引脚号（GSI）
 * @vector:     中断向量号
 * @masked:     是否屏蔽
 * @flags:      MADT ISA Override flags（用于确定极性/触发模式） */
void ioapic_set_redirection(uint8_t irq_pin, uint8_t vector, bool masked, uint16_t flags);

#endif /* SUKIOS_APIC_H */
