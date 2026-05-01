/* =============================================================================
 * SukiOS - APIC 初始化（Local APIC + I/O APIC）
 *
 * 参考：OSDev APIC, Intel SDM Vol.3 Chapter 10
 *
 * 初始化流程：
 *   1. 扩展身份映射以覆盖 MMIO 区域（0-4GB）
 *   2. 解析 ACPI MADT 获取 APIC 地址
 *   3. 通过 MSR 启用 Local APIC
 *   4. 配置 Spurious Interrupt Vector Register
 *   5. 初始化 I/O APIC（屏蔽所有重定向条目）
 * ============================================================================= */

#include "kernel/apic.h"
#include "kernel/acpi.h"
#include "kernel/tty.h"
#include "kernel/io.h"
#include <stdint.h>
#include <stdbool.h>

/* ---- 页表物理地址（与 boot.asm 一致） ---- */
#define PML4_ADDR       0x70000
#define PDPTE_A_ADDR    0x71000

/* ---- 全局 APIC MMIO 基地址 ---- */
volatile uint32_t *apic_lapic_base = NULL;
volatile uint32_t *apic_ioapic_base = NULL;

/* =========================================================================
 * 身份映射扩展
 *
 * 当前页表仅映射 0-4MB（4KB 页）和内核高半区。
 * APIC MMIO 地址通常在 0xFEC00000 / 0xFEE00000，超出已有映射范围。
 *
 * 方案：在 PDPTE_A 中添加 1GB 大页条目，将物理 1GB-4GB 映射为身份映射。
 * 使用 PS=1（Page Size）位实现，无需分配额外的 PD/PT 页。
 * ========================================================================= */
static void extend_identity_map(void)
{
    /* 将 PDPTE_A[0..3] 全部设为 1GB 大页，实现 0-4GB 完整身份映射
     *
     * 注意：覆盖 PDPTE_A[0] 会丢失 0-4MB 的 4KB 细粒度映射，
     *       但内核已在高半区运行，不再需要低地址的精细映射。
     *
     * flags: P=1(bit0) + RW=1(bit1) + PS=1(bit7) = 0x83
     * 参考：Intel SDM Vol.3 Figure 4-5: 1-GByte Page Table Entry */
    volatile uint64_t *pdpte = (volatile uint64_t *)(uintptr_t)PDPTE_A_ADDR;
    for (int i = 0; i <= 3; i++) {
        uint64_t phys = (uint64_t)i * 0x40000000ULL;  /* i * 1GB */
        pdpte[i] = phys | 0x83;
    }

    /* 刷新整个 TLB（重载 CR3，参考 Intel SDM Vol.3 4.10.4） */
    uintptr_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

/* =========================================================================
 * Local APIC 寄存器访问
 *
 * Local APIC 寄存器为 MMIO，每个寄存器 32 位，16 字节对齐。
 * 参考：Intel SDM Vol.3 10.4.3
 * ========================================================================= */
void lapic_write(uint32_t offset, uint32_t value)
{
    apic_lapic_base[offset / sizeof(uint32_t)] = value;
}

uint32_t lapic_read(uint32_t offset)
{
    return apic_lapic_base[offset / sizeof(uint32_t)];
}

/* =========================================================================
 * I/O APIC 寄存器访问
 *
 * IOAPIC 使用 indirect 寄存器访问模式：
 *   写 IOREGSEL 选择寄存器编号
 *   读写 IOWIN 获取/设置寄存器值
 *
 * 参考：Intel 82093AA datasheet, OSDev IOAPIC
 * ========================================================================= */
uint32_t ioapic_read(uint32_t reg)
{
    apic_ioapic_base[IOAPIC_REGSEL / sizeof(uint32_t)] = reg;
    return apic_ioapic_base[IOAPIC_REGWIN / sizeof(uint32_t)];
}

void ioapic_write(uint32_t reg, uint32_t value)
{
    apic_ioapic_base[IOAPIC_REGSEL / sizeof(uint32_t)] = reg;
    apic_ioapic_base[IOAPIC_REGWIN / sizeof(uint32_t)] = value;
}

/* =========================================================================
 * Local APIC 初始化
 * ========================================================================= */
static void lapic_init(uint32_t lapic_phys_addr)
{
    /* 1. 检测 CPUID 是否支持 APIC（EDX bit 9） */
    uint32_t eax, edx;
    __asm__ volatile ("cpuid" : "=a"(eax), "=d"(edx) : "a"(1) : "ebx", "ecx");
    if (!(edx & (1 << 9))) {
        tty_setcolor(VGA_RED, VGA_BLACK);
        tty_print("  ERROR: CPU does not support Local APIC!\n");
        return;
    }

    /* 2. 读取 IA32_APIC_BASE MSR，获取 LAPIC 基地址并启用 */
    uint64_t msr = rdmsr(IA32_APIC_BASE_MSR);
    uint32_t base = (uint32_t)(msr & IA32_APIC_BASE_MASK);

    /* 如果 MADT 提供了 64 位覆盖地址，优先使用 */
    if (lapic_phys_addr && lapic_phys_addr != base) {
        tty_setcolor(VGA_BROWN, VGA_BLACK);
        tty_print("  NOTE: MADT LAPIC address differs from MSR\n");
        tty_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
        base = lapic_phys_addr;
        msr = (msr & ~IA32_APIC_BASE_MASK) | (uint64_t)base;
        wrmsr(IA32_APIC_BASE_MSR, msr);
    }

    /* 确保全局启用位已设置 */
    if (!(msr & IA32_APIC_BASE_ENABLE)) {
        msr |= IA32_APIC_BASE_ENABLE;
        wrmsr(IA32_APIC_BASE_MSR, msr);
    }

    apic_lapic_base = (volatile uint32_t *)(uintptr_t)base;

    /* 3. 打印 LAPIC 信息 */
    tty_print("  [LAPIC] Base: ");
    tty_print_hex64(base);
    tty_print("  ID: ");
    tty_print_dec(lapic_read(LAPIC_ID_REG) >> 24);
    tty_print("  Version: ");
    tty_print_dec(lapic_read(LAPIC_VERSION_REG) & 0xFF);
    tty_putchar('\n');

    /* 4. 设置 Spurious Interrupt Vector Register（SVR）
     *   bit 8 = 软件启用 APIC（开始接收中断）
     *   bits 0-7 = 伪中断向量号（最低 4 位必须为 1，使用 0xFF） */
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | APIC_SPURIOUS_VECTOR);
}

/* =========================================================================
 * I/O APIC 初始化
 * ========================================================================= */
static void ioapic_init(uint32_t ioapic_phys_addr)
{
    apic_ioapic_base = (volatile uint32_t *)(uintptr_t)ioapic_phys_addr;

    /* 读取 I/O APIC 信息 */
    uint32_t id_raw = ioapic_read(IOAPIC_ID_REG);
    uint32_t ver_raw = ioapic_read(IOAPIC_VER_REG);
    uint8_t  ioapic_id = (id_raw >> 24) & 0x0F;
    uint8_t  version = ver_raw & 0xFF;
    uint8_t  max_redir = ((ver_raw >> 16) & 0xFF) + 1;

    tty_print("  [IOAPIC] Base: ");
    tty_print_hex64(ioapic_phys_addr);
    tty_print("  ID: ");
    tty_print_dec(ioapic_id);
    tty_print("  Ver: ");
    tty_print_dec(version);
    tty_print("  RedirEntries: ");
    tty_print_dec(max_redir);
    tty_putchar('\n');

    /* 屏蔽所有重定向条目（初始状态全部禁用）
     * 每个重定向条目占用 2 个 32 位寄存器 */
    for (uint32_t i = 0; i < max_redir; i++) {
        ioapic_write(IOAPIC_REDIR_BASE + i * 2, IOAPIC_REDIR_MASK);
        ioapic_write(IOAPIC_REDIR_BASE + i * 2 + 1, 0);
    }
}

/* =========================================================================
 * APIC 主初始化入口
 * ========================================================================= */
void apic_init(void)
{
    /* 1. 扩展身份映射（访问 MMIO 区域所需） */
    tty_print("[..] Extending identity map for MMIO...\n");
    extend_identity_map();
    tty_print("[OK] Identity map: 0-4GB\n");

    /* 2. 搜索 RSDP */
    tty_print("[..] Searching for RSDP...\n");
    struct acpi_rsdp *rsdp = acpi_find_rsdp();
    if (!rsdp) {
        tty_setcolor(VGA_RED, VGA_BLACK);
        tty_print("  ERROR: RSDP not found!\n");
        tty_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }
    tty_print("[OK] RSDP at ");
    tty_print_hex64((uint64_t)(uintptr_t)rsdp);
    tty_print(" rev.");
    tty_print_dec(rsdp->revision);
    tty_putchar('\n');

    /* 3. 查找 MADT 表 */
    tty_print("[..] Locating MADT table...\n");
    struct acpi_sdt_header *madt_hdr = acpi_find_table(rsdp, "APIC");
    if (!madt_hdr) {
        tty_setcolor(VGA_RED, VGA_BLACK);
        tty_print("  ERROR: MADT table not found!\n");
        tty_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    /* 4. 解析 MADT */
    struct acpi_madt_info info = acpi_parse_madt(madt_hdr);

    tty_print("[OK] MADT parsed  LAPIC: ");
    tty_print_hex64(info.lapic_addr);
    if (info.ioapic_found) {
        tty_print("  IOAPIC: ");
        tty_print_hex64(info.ioapic_addr);
    }
    tty_putchar('\n');

    /* 5. 初始化 Local APIC */
    tty_print("[..] Initializing Local APIC...\n");
    lapic_init(info.lapic_addr);
    tty_setcolor(VGA_GREEN, VGA_BLACK);
    tty_print("[OK] Local APIC enabled (spurious vector 0x");
    tty_print_dec(APIC_SPURIOUS_VECTOR);
    tty_print(")\n");
    tty_setcolor(VGA_LIGHT_GREY, VGA_BLACK);

    /* 6. 初始化 I/O APIC */
    if (info.ioapic_found) {
        tty_print("[..] Initializing I/O APIC...\n");
        ioapic_init(info.ioapic_addr);
        tty_setcolor(VGA_GREEN, VGA_BLACK);
        tty_print("[OK] I/O APIC initialized (all IRQs masked)\n");
        tty_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
    } else {
        tty_setcolor(VGA_BROWN, VGA_BLACK);
        tty_print("  WARNING: No I/O APIC found in MADT\n");
        tty_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
    }
}
