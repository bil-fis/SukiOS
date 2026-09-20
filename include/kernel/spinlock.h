/*
 * include/kernel/spinlock.h
 * -----------------------------------------------------------------------------
 * SMP 自旋锁（P0-3 锁体系）。
 *
 * 设计：
 *   - ticket lock（票号锁）：FIFO 公平，避免饥饿；lock 字段 32 位，
 *     低 16 位 = 当前放行票号(owner)，高 16 位 = 下一个发放票号(next)。
 *   - spin_lock_irqsave/spin_unlock_irqrestore：关中断 + 拿锁，用于可能在
 *     中断上下文访问的共享数据（调度器/PMM/console 等）。单核下退化为
 *     纯 cli/sti（锁必然立即成功），零额外开销，兼容旧行为。
 *   - 所有原子操作用 GCC __atomic 内建（生成 lock xadd / lock cmpxchg），
 *     "pause" 指令降低自旋功耗并让出超线程资源。
 *
 * 使用红线：
 *   1) 持锁期间严禁 schedule()/阻塞（自旋锁非睡眠锁）。
 *   2) 中断上下文可能拿的锁，进程上下文必须用 irqsave 变体拿，否则死锁
 *      （进程持锁 -> 中断打断 -> 中断再拿同一把锁 -> 永久自旋）。
 *   3) 锁序（外层 -> 内层）：sched -> kmalloc -> pmm -> console。持外层锁时
 *      只允许拿更内层的锁，反向即 ABBA 死锁（console 是最内层：任何路径的
 *      诊断打印都可能拿它，故持 console 锁期间严禁再拿其它锁）。
 */
#ifndef _SUKI_KERNEL_SPINLOCK_H
#define _SUKI_KERNEL_SPINLOCK_H

#include <kernel/types.h>
#include <kernel/serial.h>   /* 死锁诊断用 serial_writestr/dec（绕过 kprintf 锁） */
#include <kernel/console.h>  /* panic() 声明（死锁诊断末尾停机）；console.h 仅
                              * 依赖 types.h，与 spinlock.h 无循环包含关系 */
#include <kernel/percpu.h>   /* cpu_index()：每个持锁计数按 CPU 维护，避免多核误判 */

/* 持锁不可抢占保护（P0 死锁根因修复）：
 * 单核下若任务在持自旋锁期间（spin_lock_irqsave 已 cli）被 self-IPI
 * （IPI_RESCHED=0xF0，由 sched_wake 经 self-IPI 投递）抢占并 schedule() 切走，
 * 锁永不释放 -> 后续请求者自旋 50M 次 panic（disk-srv 持 'ata' 锁被切走）。
 * 标准做法：持任何自旋锁期间禁止任务切换。preempt_count[cpu] 在
 * spin_lock 时 +1、spin_unlock 时 -1；schedule() 入口若 >0 则立即返回（仅置
 * need_resched，推迟到 unlock 后的最近调度点）。单核 MAX_CPUS=1，数组退化为
 * 单元素；多核下每 CPU 独立计数，互不干扰。 */
extern uint32_t g_preempt_count[MAX_CPUS];

/* 调度器提供的「锁释放后立即抢占」入口（kernel/sched/sched.c 实现）。
 * spin_unlock 在持锁计数归零时调用：若存在被推迟的调度请求（need_resched）则
 * 立即 schedule()，使被唤醒任务在锁释放后马上运行，而非苦等下一个 100Hz 节拍。
 * 这是紧耦合 IPC（shell↔FS↔disk）往返延迟从数十毫秒降到微秒级的关键。 */
extern void sched_maybe_preempt(void);

/* 取得当前运行任务名（sched.c 提供，返回 cpu_local()->current_task->name）。
 * 用于在死锁诊断时打印持锁任务名；返回不透明字符串避免与 task.h 循环包含。 */
extern const char *get_current_task_name(void);

typedef struct spinlock {
    volatile uint32_t tickets;   /* [31:16]=next, [15:0]=owner */
    const char *name;            /* 诊断用（死锁排查时打印） */
    const char *owner_name;      /* 诊断：当前持有者任务名（死锁时打印） */
} spinlock_t;

#define SPINLOCK_INIT(nm)  { .tickets = 0, .name = (nm), .owner_name = NULL }

static inline void spinlock_init(spinlock_t *l, const char *name)
{
    l->tickets = 0;
    l->name = name;
}

static inline void cpu_relax(void)
{
    __asm__ volatile("pause" ::: "memory");
}

/* 拿锁（不处理中断状态——调用方保证上下文安全）
 *
 * 单一 ticket 协议（单核/多核通用）。注释曾计划单核退化为 cli/sti，但实测
 * 裸 cli/sti 无法阻止单核协作式任务切换（yield 不依赖中断），会导致临界区
 * 同时被两个任务进入、runqueue 损坏、系统静默冻结（比死锁更糟）。因此统一走
 * ticket 协议：单核下持锁路径只要不主动 yield，owner 票号即被正确推进，行为
 * 与原稳定版本一致。多核同样走严格 FIFO ticket 协议。 */
static inline void spin_lock(spinlock_t *l)
{
    /* 原子取票：next += 1，返回旧值的 next 字段作为本 CPU 的票号 */
    uint32_t ticket = __atomic_fetch_add(&l->tickets, 1u << 16,
                                         __ATOMIC_ACQUIRE);
    uint16_t my = (uint16_t)(ticket >> 16);
    /* 等待 owner == 本票号。带自旋上限诊断：若长时间拿不到，说明该锁被
     * 某路径持锁未释放（死锁）。严格 ticket 协议不容许"强制改写 owner"——
     * 那样会让两个 CPU 同时认为自己持锁，引入内核数据损坏。故超上限后只
     * 打印死锁诊断（绕过 kprintf 锁，直接 serial 输出）并 panic 停机，
     * 既保留可观测性，又不破坏锁协议。 */
    uint64_t spins = 0;
    while ((uint16_t)__atomic_load_n(&l->tickets, __ATOMIC_ACQUIRE) != my) {
        cpu_relax();
        if (++spins > 50000000ULL) {
            uint64_t cur = __atomic_load_n(&l->tickets, __ATOMIC_RELAXED);
            uint16_t owner = (uint16_t)cur;
            uint16_t next  = (uint16_t)(cur >> 16);
            serial_writestr("[spinlock] DEADLOCK detected lock='");
            serial_writestr(l->name ? l->name : "?");
            serial_writestr("' owner=");
            serial_write_dec((uint64_t)owner);
            serial_writestr(" next=");
            serial_write_dec((uint64_t)next);
            serial_writestr(" my=");
            serial_write_dec((uint64_t)my);
            serial_writestr(" held_by='");
            serial_writestr(l->owner_name ? l->owner_name : "?");
            serial_writestr("'\n");
            panic("[spinlock] deadlock on '%s' (owner=%u next=%u my=%u held_by=%s)",
                  l->name ? l->name : "?", (unsigned)owner, (unsigned)next,
                  (unsigned)my, (l->owner_name ? l->owner_name : "?"));
        }
    }
    l->owner_name = get_current_task_name();   /* 记录持锁任务名（诊断用） */
    /* 进入原子/持锁区：禁止调度器在持锁期间抢占本任务（死锁根因修复）。
     * self-IPI(0xF0) 经 sched_wake 投递后，若直接 schedule() 会把持锁任务切走，
     * 锁永不释放 -> 后续请求者自旋 panic。持锁期间累计计数，schedule() 入口
     * 据此跳过切换，待 unlock 后再由 need_resched 触发。 */
    g_preempt_count[cpu_index()]++;
}

static inline void spin_unlock(spinlock_t *l)
{
    /* 退出原子/持锁区；必须严格配平 spin_lock 的 ++，否则计数失衡 */
    uint32_t _idx = cpu_index();
    if (g_preempt_count[_idx]) {
        g_preempt_count[_idx]--;
    }
    l->owner_name = NULL;                          /* 释放：清除持锁者记录 */
    /* owner += 1（只动低 16 位；直接对低半字做原子加） */
    __atomic_fetch_add((volatile uint16_t *)&l->tickets, 1,
                       __ATOMIC_RELEASE);

    /* 锁释放后立即抢占点：本 CPU 持锁计数归零时，若此前有被推迟的调度请求
     * （need_resched），马上 schedule()。此前该请求要等到下一个 100Hz 节拍才被处理，
     * 使紧耦合 IPC 每跳延迟被放大到数十毫秒（实测每 3500B 读块 4~300ms，大文件读取
     * 近乎“挂死”）。函数内部自带 g_sched_ready/持锁计数/need_resched 三重检查，故在
     * 任何上下文调用都安全。 */
    if (g_preempt_count[_idx] == 0) {
        sched_maybe_preempt();
    }
}

/* 尝试拿锁：成功返回 true。仅当无人排队且立即可得才成功。 */
static inline bool spin_trylock(spinlock_t *l)
{
    uint32_t cur = __atomic_load_n(&l->tickets, __ATOMIC_RELAXED);
    uint16_t owner = (uint16_t)cur;
    uint16_t next  = (uint16_t)(cur >> 16);
    if (owner != next) {
        return false;             /* 有人持有或排队 */
    }
    uint32_t want = ((uint32_t)(next + 1) << 16) | owner;
    return __atomic_compare_exchange_n(&l->tickets, &cur, want, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

/* ---- 关中断变体：返回进入前的 RFLAGS，恢复时按其 IF 位决定是否 sti ---- */
static inline uint64_t spin_lock_irqsave(spinlock_t *l)
{
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(l);
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags)
{
    spin_unlock(l);
    if (flags & (1UL << 9)) {
        __asm__ volatile("sti" ::: "memory");
    }
}

#endif /* _SUKI_KERNEL_SPINLOCK_H */
