/* =============================================================================
 * SukiOS - 64 位 GDT 初始化
 *
 * 参考：OSDev GDT Tutorial, AMD64 APM Vol.2 4.8.1-4.8.2
 *
 * 在 long mode 中，段基地址和段界限被忽略（平坦模型），
 * 但 TSS 仍然需要正确设置。
 * ============================================================================= */

#include "kernel/gdt.h"
#include "kernel/tty.h"
#include "kernel/io.h"          /* 使用统一的 rdmsr / wrmsr */

/* GDT 表（7 个 64 位条目） */
static uint64_t gdt_entries[GDT_ENTRIES];

/* TSS 实例（BSS 段，链接器初始化为零） */
struct tss_entry kernel_tss;

/* ISR 存根中的栈符号 */
extern uint8_t double_fault_stack_top;
extern uint8_t nmi_stack_top;
extern uint8_t machine_check_stack_top;
extern uint8_t kernel_interrupt_stack_top;

/* ---- GDT 编码辅助函数 ---- */
static inline uint64_t make_seg(uint32_t limit, uint32_t base,
                                uint8_t access, uint8_t flags)
{
    uint64_t desc = 0;
    desc |= (limit & 0xFFFF);
    desc |= ((uint64_t)(base & 0xFFFF)) << 16;
    desc |= ((uint64_t)(base >> 16) & 0xFF) << 32;
    desc |= ((uint64_t)access) << 40;
    desc |= ((uint64_t)(limit >> 16) & 0x0F) << 48;
    desc |= ((uint64_t)(flags & 0x0F)) << 52;
    desc |= ((uint64_t)(base >> 24) & 0xFF) << 56;
    return desc;
}

static void make_tss_desc(uint64_t *low, uint64_t *high,
                          uint64_t base, uint32_t limit)
{
    *low = 0;
    *low |= (limit & 0xFFFF);
    *low |= ((uint64_t)(base & 0xFFFF)) << 16;
    *low |= ((uint64_t)(base >> 16) & 0xFF) << 32;
    *low |= ((uint64_t)0x89) << 40;
    *low |= ((uint64_t)(limit >> 16) & 0x0F) << 48;
    *low |= ((uint64_t)(base >> 24) & 0xFF) << 56;

    *high = (base >> 32) & 0xFFFFFFFF;
}

static void load_gdt(uint64_t *gdt_base, uint16_t gdt_limit)
{
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) gdtr;
    gdtr.limit = gdt_limit;
    gdtr.base = (uint64_t)gdt_base;

    __asm__ volatile (
        "lgdt %[gdtr]\n\t"
        "pushq $0x08\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "movw $0x10, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        :
        : [gdtr] "m"(gdtr)
        : "rax", "memory"
    );
}

void gdt_init(void)
{
    for (int i = 0; i < GDT_ENTRIES; i++)
        gdt_entries[i] = 0;

    gdt_entries[0] = 0;

    gdt_entries[1] = make_seg(0xFFFFF, 0, 0x9A, 0xA);
    gdt_entries[2] = make_seg(0xFFFFF, 0, 0x92, 0xC);
    gdt_entries[3] = make_seg(0xFFFFF, 0, 0xFA, 0xA);
    gdt_entries[4] = make_seg(0xFFFFF, 0, 0xF2, 0xC);

    kernel_tss.rsp0 = (uint64_t)&kernel_interrupt_stack_top;
    kernel_tss.ist1 = (uint64_t)&double_fault_stack_top;
    kernel_tss.ist2 = (uint64_t)&nmi_stack_top;
    kernel_tss.ist3 = (uint64_t)&machine_check_stack_top;
    kernel_tss.iopb_offset = sizeof(struct tss_entry);

    make_tss_desc(&gdt_entries[5], &gdt_entries[6],
                  (uint64_t)&kernel_tss, sizeof(struct tss_entry) - 1);

    load_gdt(gdt_entries, sizeof(gdt_entries) - 1);
    __asm__ volatile ("ltr %w0" : : "r"(GDT_SELECTOR_TSS));

    /* ---- 配置 syscall/sysret MSR ---- */
    uint64_t star = ((uint64_t)GDT_SELECTOR_KERNEL_CS << 48) |
                    ((uint64_t)(GDT_SELECTOR_USER_CS - 0x10) << 32);
    wrmsr(MSR_IA32_STAR, star);
    wrmsr(MSR_IA32_FMASK, 0x200);   /* 清除 IF */

    /* 启用 EFER.SCE */
    uint64_t efer = rdmsr(MSR_IA32_EFER);
    efer |= EFER_SCE;
    wrmsr(MSR_IA32_EFER, efer);
}