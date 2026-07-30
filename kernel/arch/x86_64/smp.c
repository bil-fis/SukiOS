/*
 * kernel/arch/x86_64/smp.c
 * -----------------------------------------------------------------------------
 * SMP 启动与 IPI 实现（见 include/kernel/smp.h）。
 *
 * AP 启动序列（Intel MP Spec / SDM Vol.3 8.4.4）：
 *   1) 拷贝跳板（ap_boot.S）到物理 0x8000（低 1MB 已由 PMM 保留，无冲突）；
 *   2) 填邮箱：CR3=内核 PML4、AP 专属 16KB 栈顶、入口 ap_main、CPU 索引；
 *   3) INIT（assert+deassert）-> 等 10ms -> SIPI(向量 0x08) -> 等 -> 必要时
 *      第二次 SIPI；轮询 percpu.online 握手（超时 100ms 判失败）；
 *   4) 串行逐个启动（共享一个邮箱），全部完成后打印在线拓扑。
 *
 * 调用关系：kmain() -> smp_init()（clock_init 之后，需 TSC 延时）。
 *           isr_dispatch -> ipi_*_handler（向量 0xF0..0xF2）。
 */
#include <kernel/smp.h>
#include <kernel/percpu.h>
#include <kernel/apic.h>
#include <kernel/acpi.h>
#include <kernel/clock.h>
#include <kernel/interrupts.h>
#include <kernel/gdt.h>
#include <kernel/security.h>
#include <kernel/syscall.h>
#include <kernel/task.h>
#include <kernel/console.h>
#include <kernel/string.h>
#include <mm/kmalloc.h>
#include <mm/kstack.h>     /* P0-R5：AP 引导/空闲栈同样使用守卫页栈 */
#include <mm/vmm.h>

#define AP_TRAMP_PHYS   0x8000UL
#define AP_STACK_BYTES  KSTACK_BYTES   /* 16KB，栈底下方带未映射守卫页 */

/* ap_boot.S 导出的跳板边界与邮箱标号（内核 VMA 内的地址，用于算偏移） */
extern char ap_tramp_start[], ap_tramp_end[];
extern char ap_mb_cr3[], ap_mb_stack[], ap_mb_entry[], ap_mb_idx[];

static uint32_t g_online = 1;    /* BSP 永远在线 */

/* ---- TSC 忙等延时（微秒级；clock_init 已校准 TSC）---- */
static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return ((uint64_t)hi << 32) | lo;
}

static void udelay(uint64_t usec)
{
    uint64_t hz = clock_tsc_freq_hz();
    if (hz == 0) {
        /* TSC 未校准的极端回退：粗略循环 */
        for (volatile uint64_t i = 0; i < usec * 1000; i++) { }
        return;
    }
    uint64_t end = rdtsc() + (hz / 1000000ULL) * usec;
    while (rdtsc() < end) {
        __asm__ volatile("pause" ::: "memory");
    }
}

/* ---- IPI 处理器 ---- */

static void ipi_resched_handler(registers_t *r)
{
    (void)r;
    /* 骨架：AP 尚无 runqueue。收到即返回（EOI 已由 isr_dispatch 发送）。 */
    cpu_local()->ticks++;
}

static void ipi_tlb_handler(registers_t *r)
{
    (void)r;
    /* 整体 TLB 刷新：重载 CR3。开销大于 invlpg 单页，但协议最简且绝对正确；
     * 精确 shootdown（传地址+代际计数）随多核调度阶段引入。 */
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    cpu_local()->ticks++;
}

static void ipi_halt_handler(registers_t *r)
{
    (void)r;
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

/* ---- AP 的 C 入口（ap_boot.S 长模式段 jmp 至此，rdi=CPU 逻辑索引）---- */
void ap_main(uint64_t idx)
{
    gdt_init_ap((uint32_t)idx);        /* per-CPU GDT+TSS（IST 可用） */
    idt_load_ap();                     /* 共享同一张 IDT */
    uint8_t apic = lapic_init();       /* 使能本 CPU 的 LAPIC（LVT 全屏蔽） */
    percpu_install((uint32_t)idx, apic);

    /* P0-R1：在本 AP 上应用与 BSP 一致的安全控制位（SMEP/SMAP/UMIP/NXE）。
     * 单核假设下 AP 漏设会让用户态可执行内核页/读内核数据，是真实缺口。 */
    cpu_apply_security_features();

    /* P0-R1：syscall MSR（LSTAR/STAR/FMASK/EFER.SCE）是每核私有的，
     * AP 必须自行初始化 —— 否则本核用户任务一执行 syscall 即双重故障。 */
    syscall_init_cpu();

    /* 握手：告知 BSP 本 AP 已完全就绪（release 语义确保上面全部可见） */
    __atomic_store_n(&g_percpu[idx].online, 1, __ATOMIC_RELEASE);

    /* P0-R1：每 AP 启动自己的 LAPIC 周期定时器（100Hz），本核节拍触发
     * sched_tick -> schedule()，从而真正参与对称多核调度（不再空转）。 */
    lapic_timer_start((uint8_t)IRQ0, 100);

    /* 进入 idle 循环：开中断；无任务时 hlt 省电，被 IPI/定时器唤醒后调度。 */
    interrupts_enable();
    for (;;) {
        g_percpu[idx].in_idle = 1;
        __asm__ volatile("hlt");
        g_percpu[idx].in_idle = 0;
        schedule();
    }
}

uint32_t smp_init(void)
{
    /* IPI 向量处理器（BSP/AP 共享 IDT 与 handler 表） */
    register_interrupt_handler(IPI_RESCHED,   ipi_resched_handler);
    register_interrupt_handler(IPI_TLB_FLUSH, ipi_tlb_handler);
    register_interrupt_handler(IPI_HALT,      ipi_halt_handler);

    if (g_acpi.lapic_count <= 1) {
        kprintf("[smp] single CPU system (MADT lapic_count=%u)\n",
                (unsigned)g_acpi.lapic_count);
        return 1;
    }

    /* 1) 跳板拷贝到 0x8000 */
    uint64_t tramp_size = (uint64_t)(ap_tramp_end - ap_tramp_start);
    memcpy(PHYS_TO_VIRT(AP_TRAMP_PHYS), ap_tramp_start, tramp_size);
    kprintf("[smp] trampoline copied to %p (%lu bytes)\n",
            (void *)AP_TRAMP_PHYS, (unsigned long)tramp_size);

    /* 邮箱字段在跳板拷贝中的运行时地址 */
    volatile uint64_t *mb_cr3 = (volatile uint64_t *)
        PHYS_TO_VIRT(AP_TRAMP_PHYS + (uint64_t)(ap_mb_cr3 - ap_tramp_start));
    volatile uint64_t *mb_stack = (volatile uint64_t *)
        PHYS_TO_VIRT(AP_TRAMP_PHYS + (uint64_t)(ap_mb_stack - ap_tramp_start));
    volatile uint64_t *mb_entry = (volatile uint64_t *)
        PHYS_TO_VIRT(AP_TRAMP_PHYS + (uint64_t)(ap_mb_entry - ap_tramp_start));
    volatile uint64_t *mb_idx = (volatile uint64_t *)
        PHYS_TO_VIRT(AP_TRAMP_PHYS + (uint64_t)(ap_mb_idx - ap_tramp_start));

    uint8_t bsp = lapic_id();
    uint32_t idx = 1;

    for (uint32_t i = 0; i < g_acpi.lapic_count && idx < MAX_CPUS; i++) {
        uint8_t target = (uint8_t)g_acpi.madt_lapic_id[i];
        if (target == bsp) {
            continue;
        }

        /* P0-R5：AP 引导栈改用守卫页栈——该栈是 AP idle 循环的实际执行栈
         * （schedule 首次从 idle 切出时 RSP 保存于 idle 任务），溢出可诊断。 */
        uint64_t stack = kstack_alloc();
        if (!stack) {
            kprintf("[smp] cpu%u: stack alloc failed, skipping\n", (unsigned)idx);
            continue;
        }

        /* P0-R1：为 AP 预建 idle 任务并入其运行队列，置 percpu.current_task。
         * AP 上电后读 percpu 即得其 idle，首次 schedule() 即可参与调度。 */
        if (!sched_create_idle(idx)) {
            kprintf("[smp] cpu%u: idle alloc failed, skipping\n", (unsigned)idx);
            kstack_free(stack);
            continue;
        }

        /* 2) 填邮箱（volatile 写 + 后续 IPI 前的 sfence 语义由 wrmsr/MMIO 保证） */
        *mb_cr3   = vmm_kernel_pml4();
        *mb_stack = stack + AP_STACK_BYTES;
        *mb_entry = (uint64_t)ap_main;
        *mb_idx   = idx;
        g_percpu[idx].online = 0;
        g_percpu[idx].kstack_top = stack + AP_STACK_BYTES;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        /* 3) INIT-SIPI-SIPI */
        lapic_send_init(target);
        udelay(10000);                              /* 10ms */
        lapic_send_startup(target, AP_TRAMP_PHYS >> 12);
        udelay(300);
        if (!g_percpu[idx].online) {
            lapic_send_startup(target, AP_TRAMP_PHYS >> 12);
        }

        /* 握手：最多等 100ms */
        uint32_t waited = 0;
        while (!__atomic_load_n(&g_percpu[idx].online, __ATOMIC_ACQUIRE) &&
               waited < 100000) {
            udelay(100);
            waited += 100;
        }
        if (g_percpu[idx].online) {
            g_online++;
            kprintf("[smp] cpu%u online (lapic_id=%u)\n",
                    (unsigned)idx, (unsigned)target);
            idx++;
        } else {
            kprintf("[smp] cpu%u (lapic_id=%u) FAILED to start\n",
                    (unsigned)idx, (unsigned)target);
            kstack_free(stack);
        }
    }

    kprintf("[smp] %u/%u CPUs online (BSP lapic_id=%u)\n",
            (unsigned)g_online, (unsigned)g_acpi.lapic_count, (unsigned)bsp);

    /* IPI 自检：广播一次 RESCHED，确认每个在线 AP 的 ticks 递增
     * （证明 AP 的 IDT/LAPIC/中断路径全通，而非仅仅完成握手）。 */
    if (g_online > 1) {
        uint64_t before[MAX_CPUS];
        for (uint32_t i = 1; i < g_online; i++) {
            before[i] = g_percpu[i].ticks;
        }
        lapic_broadcast_ipi(IPI_RESCHED);
        udelay(2000);                               /* 2ms 足够 hlt 唤醒 */
        uint32_t acked = 0;
        for (uint32_t i = 1; i < g_online; i++) {
            if (g_percpu[i].ticks > before[i]) {
                acked++;
            }
        }
        kprintf("[smp] IPI selftest: %u/%u APs acked RESCHED\n",
                (unsigned)acked, (unsigned)(g_online - 1));
    }
    return g_online;
}

uint32_t smp_online_count(void)
{
    return g_online;
}

void smp_tlb_shootdown(void)
{
    if (g_online <= 1) {
        return;
    }
    lapic_broadcast_ipi(IPI_TLB_FLUSH);
}

void smp_halt_others(void)
{
    if (g_online <= 1) {
        return;
    }
    lapic_broadcast_ipi(IPI_HALT);
}
