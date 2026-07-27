/*
 * kernel/arch/x86_64/ioapic.c
 * -----------------------------------------------------------------------------
 * I/O APIC 实现（见 include/kernel/ioapic.h）。
 *
 * 调用关系：kmain() -> acpi_init() 之后 -> ioapic_init() -> ioapic_route()。
 *
 * 寄存器访问：IOAPIC 是 MMIO（基址来自 ACPI MADT，默认 0xFEC00000），
 * 经 PHYS_TO_VIRT 访问。索引寄存器 IOREGSEL(0x00) 选表项，数据窗 IOWIN(0x10)
 * 读写 64 位重定向条目（低 32 在 0x10+2*n，高 32 在 0x10+2*n+1）。
 */
#include <kernel/ioapic.h>
#include <kernel/acpi.h>
#include <kernel/console.h>

#define IOAPIC_REG_SEL  0x00
#define IOAPIC_REG_WIN  0x10
#define IOAPIC_ID       0x00
#define IOAPIC_VER      0x01
#define IOAPIC_REDIR(n) (0x10 + (n) * 2)   /* 重定向表第 n 项（低 32） */

static uint64_t g_ioapic_base = 0xFEC00000ULL;
static uint32_t g_max_redir = 23;    /* 由版本寄存器得到条目数-1 */
static uint8_t  g_dest_lapic = 0;    /* 目标 LAPIC ID（BSP，单核=0） */

static inline void ioapic_write_reg(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)((uint8_t *)PHYS_TO_VIRT(g_ioapic_base) + IOAPIC_REG_SEL) = reg;
    *(volatile uint32_t *)((uint8_t *)PHYS_TO_VIRT(g_ioapic_base) + IOAPIC_REG_WIN) = val;
}

static inline uint32_t ioapic_read_reg(uint32_t reg)
{
    *(volatile uint32_t *)((uint8_t *)PHYS_TO_VIRT(g_ioapic_base) + IOAPIC_REG_SEL) = reg;
    return *(volatile uint32_t *)((uint8_t *)PHYS_TO_VIRT(g_ioapic_base) + IOAPIC_REG_WIN);
}

void ioapic_set_dest(uint8_t lapic_id)
{
    g_dest_lapic = lapic_id;
}

bool ioapic_init(void)
{
    if (g_acpi.ioapic_phys) {
        g_ioapic_base = g_acpi.ioapic_phys;
    } else {
        kprintf("[ioapic] no IOAPIC in MADT; assuming default 0xFEC00000\n");
    }

    uint32_t ver = ioapic_read_reg(IOAPIC_VER);
    g_max_redir = (ver >> 16) & 0xFF;   /* 最大重定向条目号 = (count-1) */
    kprintf("[ioapic] found @ %p: id=0x%x version=0x%x max_redir=%u\n",
            (void *)g_ioapic_base, (unsigned)ioapic_read_reg(IOAPIC_ID),
            (unsigned)ver, (unsigned)g_max_redir);

    /* 屏蔽全部重定向条目（清零低 32 的 mask 位之外的所有位，含 vector=0） */
    for (uint32_t i = 0; i <= g_max_redir; i++) {
        ioapic_write_reg(IOAPIC_REDIR(i), 1u << 16);     /* mask=1 */
        ioapic_write_reg(IOAPIC_REDIR(i) + 1, 0);         /* 高 32 清零 */
    }
    return true;
}

void ioapic_route(uint32_t gsi, uint8_t vector,
                  bool level, bool active_low, uint8_t dest)
{
    if (gsi > g_max_redir) {
        kprintf("[ioapic] gsi %u out of range (max %u)\n",
                (unsigned)gsi, (unsigned)g_max_redir);
        return;
    }
    uint32_t lo = (uint32_t)vector;        /* bits 0-7: 向量 */
    if (level) {
        lo |= (1u << 15);                  /* 触发模式：1=电平 */
    }
    /* 触发极性：1=低电平有效（active_low） */
    if (active_low) {
        lo |= (1u << 13);
    }
    /* 目标模式：0=物理（dest 为 LAPIC ID） */
    lo &= ~(1u << 11);
    lo &= ~(1u << 16);                     /* mask=0 放开 */

    uint32_t hi = ((uint32_t)dest & 0xFF) << 24;   /* 目标 LAPIC ID（物理） */

    ioapic_write_reg(IOAPIC_REDIR(gsi), lo);
    ioapic_write_reg(IOAPIC_REDIR(gsi) + 1, hi);
}

void ioapic_mask(uint32_t gsi)
{
    if (gsi > g_max_redir) {
        return;
    }
    uint32_t lo = ioapic_read_reg(IOAPIC_REDIR(gsi));
    ioapic_write_reg(IOAPIC_REDIR(gsi), lo | (1u << 16));
}

void ioapic_unmask(uint32_t gsi)
{
    if (gsi > g_max_redir) {
        return;
    }
    uint32_t lo = ioapic_read_reg(IOAPIC_REDIR(gsi));
    ioapic_write_reg(IOAPIC_REDIR(gsi), lo & ~(1u << 16));
}
