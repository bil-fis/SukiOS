/*
 * kernel/arch/x86_64/percpu.c
 * -----------------------------------------------------------------------------
 * per-CPU 数据实现（见 include/kernel/percpu.h）。
 *
 * 调用关系：kmain()（BSP，lapic_init 之后）与 ap_main()（各 AP）分别调用
 *           percpu_install()；全内核经 cpu_index()/cpu_local() 取本 CPU 槽。
 */
#include <kernel/percpu.h>
#include <kernel/console.h>

percpu_t g_percpu[MAX_CPUS];

/* percpu 就绪标志：BSP 安装完成前，cpu_index() 一律返回 0。
 * 否则 GS_BASE 尚为 0 时读 %gs:0 会取到物理页 0（IVT）的垃圾数据。 */
static volatile bool g_percpu_ready = false;

/*
 * wrmsr 封装（手册 9.2 特权指令隔离）。
 * 输入约束：%ecx = MSR 号，%eax = 低 32 位，%edx = 高 32 位。
 * Clobber：内存屏障（防止编译器跨 MSR 写重排 percpu 访问）。
 */
static __attribute__((noinline)) void wrmsr_isolated(uint32_t msr, uint64_t val)
{
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi) : "memory");
}

#define MSR_GS_BASE 0xC0000101u

void percpu_install(uint32_t cpu_index_, uint32_t lapic_id)
{
    percpu_t *p = &g_percpu[cpu_index_];
    p->cpu_index = cpu_index_;
    p->lapic_id  = lapic_id;
    p->ticks     = 0;

    /* GS_BASE 指向本 CPU 槽。此后 %gs:0 = cpu_index。
     * 注意：绝不能再重载 gs 段选择子（会清 GS_BASE）；gdt_load 仅在
     * percpu_install 之前执行（BSP: gdt_init；AP: gdt_init_ap）。 */
    wrmsr_isolated(MSR_GS_BASE, (uint64_t)p);

    if (cpu_index_ == 0) {
        g_percpu_ready = true;
    }
    kprintf("[percpu] cpu%u installed (lapic_id=%u, slot=%p)\n",
            (unsigned)cpu_index_, (unsigned)lapic_id, (void *)p);
}

uint32_t cpu_index(void)
{
    if (!g_percpu_ready) {
        return 0;
    }
    uint32_t v;
    __asm__ volatile("movl %%gs:0, %0" : "=r"(v));
    return v;
}

percpu_t *cpu_local(void)
{
    return &g_percpu[cpu_index()];
}
