/*
 * kernel/sched/futex.c
 * -----------------------------------------------------------------------------
 * futex（fast userspace mutex）内核实现。详见 include/kernel/futex.h。
 *
 * 本文件是被 kernel/ 自动发现的 .c 源（C_SRCS := find kernel -name '*.c'），
 * 无需在 Makefile 手动登记。
 *
 * 线程库（用户态 pthread.c）的 mutex/cond 即建于此：加锁失败时在 futex 字上
 * futex_wait，释放锁时 futex_wake 一个等待者。pthread_join 亦用一把 futex 字
 * 等待目标线程退出（配合内核 clear_child_tid 清零 + 唤醒）。
 */
#include <kernel/types.h>
#include <kernel/task.h>
#include <kernel/futex.h>
#include <kernel/spinlock.h>
#include <kernel/syscall.h>   /* copy_from_user */
#include <mm/kmalloc.h>       /* kzalloc / kfree */

#define FUTEX_BUCKETS 256

typedef struct futex_waiter {
    struct futex_waiter *next;
    task_t  *task;
    uint64_t uaddr;   /* 用户虚拟地址 */
    uint64_t cr3;     /* 地址空间标识（共享 cr3 的线程视为同一空间） */
} futex_waiter_t;

static futex_waiter_t *g_futex_buckets[FUTEX_BUCKETS];
static spinlock_t g_futex_lock = SPINLOCK_INIT("futex");

static inline uint32_t futex_hash(uint64_t cr3, uint64_t uaddr)
{
    uint64_t h = (cr3 ^ uaddr) * 0x9E3779B1UL;
    return (uint32_t)(h ^ (h >> 32)) & (FUTEX_BUCKETS - 1);
}

int futex_wait(uint32_t *uaddr, uint32_t val, uint64_t timeout_ns)
{
    (void)timeout_ns;   /* 暂不支持超时：阻塞至被唤醒 */
    task_t *cur = sched_current();

    /* 1) 先确认 futex 字当前值 */
    uint32_t cur_val;
    if (copy_from_user(&cur_val, uaddr, sizeof(uint32_t)) != sizeof(uint32_t))
        return -SUKI_EFAULT;
    if (cur_val != val)
        return -SUKI_EAGAIN;

    /* 2) 分配等待节点 */
    futex_waiter_t *w = (futex_waiter_t *)kzalloc(sizeof(futex_waiter_t));
    if (!w)
        return -SUKI_ENOMEM;
    w->task  = cur;
    w->uaddr = (uint64_t)uaddr;
    w->cr3   = cur->cr3;

    uint32_t b = futex_hash(cur->cr3, (uint64_t)uaddr);

    /* 3) 在持锁区间内完成「入桶 + 二次校验 + 置 BLOCKED」，杜绝丢失唤醒 */
    uint64_t f = spin_lock_irqsave(&g_futex_lock);
    w->next = g_futex_buckets[b];
    g_futex_buckets[b] = w;

    /* 二次校验：若此刻值已被改写，则取消等待直接返回 -EAGAIN */
    if (copy_from_user(&cur_val, uaddr, sizeof(uint32_t)) != sizeof(uint32_t))
        cur_val = val ^ 1;   /* 读取失败也算不匹配，回滚 */
    if (cur_val != val) {
        g_futex_buckets[b] = w->next;
        spin_unlock_irqrestore(&g_futex_lock, f);
        kfree(w);
        return -SUKI_EAGAIN;
    }

    /* 置阻塞并脱离运行队列；随后让出 CPU */
    cur->state = BLOCKED;
    cur->in_rq = false;
    spin_unlock_irqrestore(&g_futex_lock, f);

    schedule();   /* 被 futex_wake 唤醒后从此处返回 */

    /* 4) 唤醒后从桶中摘除并释放节点（节点由本任务自己释放，futex_wake 不释放） */
    f = spin_lock_irqsave(&g_futex_lock);
    futex_waiter_t **pp = &g_futex_buckets[b];
    while (*pp) {
        if (*pp == w) { *pp = w->next; break; }
        pp = &(*pp)->next;
    }
    spin_unlock_irqrestore(&g_futex_lock, f);
    kfree(w);
    return 0;
}

int futex_wake(uint32_t *uaddr, int n)
{
    task_t *cur = sched_current();
    uint32_t b = futex_hash(cur->cr3, (uint64_t)uaddr);
    int woken = 0;

    /* 先持锁收集待唤醒任务，再释放 g_futex_lock 后才调用 sched_wake。
     * 原因：sched_wake 内部会获取 g_sched_lock；若持 g_futex_lock 同时获取
     * g_sched_lock，会与 task_exit_current（持 g_sched_lock 后若再取 g_futex_lock）
     * 形成反向锁序死锁。故此处改为「收集 -> 放锁 -> 唤醒」，两把锁永不嵌套。 */
    task_t *wake_list[64];
    int cnt = 0;
    uint64_t f = spin_lock_irqsave(&g_futex_lock);
    futex_waiter_t **pp = &g_futex_buckets[b];
    while (*pp && (n <= 0 || cnt < n) && cnt < 64) {
        futex_waiter_t *w = *pp;
        if (w->uaddr == (uint64_t)uaddr && w->cr3 == cur->cr3) {
            *pp = w->next;                 /* 从桶中摘除（节点由等待方自己释放） */
            wake_list[cnt++] = w->task;
        } else {
            pp = &w->next;
        }
    }
    spin_unlock_irqrestore(&g_futex_lock, f);

    for (int i = 0; i < cnt; i++) {
        sched_wake(wake_list[i]);
        woken++;
    }
    return woken;
}
