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

typedef struct spinlock {
    volatile uint32_t tickets;   /* [31:16]=next, [15:0]=owner */
    const char *name;            /* 诊断用（死锁排查时打印） */
} spinlock_t;

#define SPINLOCK_INIT(nm)  { 0, nm }

static inline void spinlock_init(spinlock_t *l, const char *name)
{
    l->tickets = 0;
    l->name = name;
}

static inline void cpu_relax(void)
{
    __asm__ volatile("pause" ::: "memory");
}

/* 拿锁（不处理中断状态——调用方保证上下文安全） */
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
     * 既保留可观测性，又不破坏锁协议。单核下 spin_lock 必然立即成功，
     * 该上限分支在正常的单核生产场景永不触发。 */
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
            serial_writestr("\n");
            panic("[spinlock] deadlock on '%s' (owner=%u next=%u my=%u)",
                  l->name ? l->name : "?", (unsigned)owner, (unsigned)next,
                  (unsigned)my);
        }
    }
}

static inline void spin_unlock(spinlock_t *l)
{
    /* owner += 1（只动低 16 位；直接对低半字做原子加） */
    __atomic_fetch_add((volatile uint16_t *)&l->tickets, 1,
                       __ATOMIC_RELEASE);
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
