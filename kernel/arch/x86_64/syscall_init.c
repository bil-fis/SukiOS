/*
 * kernel/arch/x86_64/syscall_init.c
 * -----------------------------------------------------------------------------
 * syscall/sysret MSR 初始化（手册 5.1）。
 *
 * 需要写入的 MSR：
 *   IA32_EFER  (0xC0000080) : 置位 SCE (bit0)，启用 syscall 指令
 *   IA32_STAR  (0xC0000081) : [47:32]=内核 CS 基 (0x08)（sysret 域不用，
 *                             因为返回路径采用 iretq）
 *   IA32_LSTAR (0xC0000082) : syscall 的 64 位入口 RIP
 *   IA32_FMASK (0xC0000084) : syscall 进入时清除的 RFLAGS 位（IF|TF|DF）
 *
 * 调用关系：kmain() -> syscall_init()。之后 Ring3 才能执行 syscall。
 */
#include <kernel/syscall.h>
#include <kernel/console.h>
#include <kernel/percpu.h>   /* MAX_CPUS：per-CPU syscall 数组维度 */

#define MSR_EFER   0xC0000080U
#define MSR_STAR   0xC0000081U
#define MSR_LSTAR  0xC0000082U
#define MSR_FMASK  0xC0000084U

#define EFER_SCE   (1UL << 0)
#define EFER_NXE   (1UL << 11)   /* 启用 NX 位；否则 PTE bit63 是保留位会引发缺页 */

extern void syscall_entry(void);   /* syscall_entry.S */

/* P0-R1：syscall 快速路径的 per-CPU 全局量（[MAX_CPUS] 数组）。
 * syscall_entry.S 经 %gs:0 取 cpu 索引后索引这些数组，避免多核并发
 * syscall 互相踩踏（全局单例会让 CPU1 用 CPU0 的内核栈/暂存）。 */
uint64_t  g_syscall_kstack[MAX_CPUS];
uint64_t *g_scratch[MAX_CPUS];
uint64_t  g_utmp_rsp[MAX_CPUS];

/* ---- 特权指令隔离封装（手册 9.2）---- */

/*
 * wrmsr_isolated: 写 MSR。
 * 输入约束："c"(msr) -> ECX = MSR 号；"a"(lo) -> EAX = 低 32 位；
 *           "d"(hi) -> EDX = 高 32 位。
 * 输出约束：无。
 * Clobber List："memory" —— 阻止编译器跨 MSR 写重排内存访问。
 * wrmsr 无其它隐式寄存器破坏。
 */
static __attribute__((noinline)) void wrmsr_isolated(uint32_t msr, uint64_t val)
{
    uint32_t lo = (uint32_t)(val & 0xFFFFFFFFU);
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi) : "memory");
}

/*
 * rdmsr_isolated: 读 MSR。
 * 输入约束："c"(msr) -> ECX = MSR 号。
 * 输出约束："=a"(lo) EAX 低 32 位；"=d"(hi) EDX 高 32 位。
 * Clobber List：无（rdmsr 只写 EAX/EDX，已由输出约束表达）。
 */
static __attribute__((noinline)) uint64_t rdmsr_isolated(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* 在当前 CPU 上写 syscall MSR 组。LSTAR/STAR/FMASK/EFER.SCE 都是每核私有
 * MSR —— 只在 BSP 初始化会让 AP 上的用户任务执行 syscall 时直接 #UD/#DF
 * （P0-R1 真实故障：cpu2/cpu3 的 fs-server/input-server 双重故障）。
 * BSP 由 syscall_init() 调用；每个 AP 在 ap_main() 中调用。 */
void syscall_init_cpu(void)
{
    /* 1. EFER.SCE=1 启用 syscall；EFER.NXE=1 启用 NX（用户栈页带 PTE_NX） */
    uint64_t efer = rdmsr_isolated(MSR_EFER);
    wrmsr_isolated(MSR_EFER, efer | EFER_SCE | EFER_NXE);

    /* 2. STAR：syscall 进入时 CS=0x08, SS=0x10（[47:32]=0x08） */
    wrmsr_isolated(MSR_STAR, (uint64_t)0x08 << 32);

    /* 3. LSTAR：64 位 syscall 入口地址 */
    wrmsr_isolated(MSR_LSTAR, (uint64_t)syscall_entry);

    /* 4. FMASK：进入内核即清 IF(0x200)|TF(0x100)|DF(0x400) */
    wrmsr_isolated(MSR_FMASK, 0x700);
}

void syscall_init(void)
{
    syscall_init_cpu();
    kprintf("[syscall] LSTAR=%p EFER.SCE=1 FMASK=0x700 (per-CPU MSRs)\n",
            (void *)syscall_entry);
}
