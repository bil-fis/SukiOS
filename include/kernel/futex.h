/*
 * include/kernel/futex.h
 * -----------------------------------------------------------------------------
 * 内核态 futex（fast userspace mutex）核心原语。
 *
 * 设计要点：
 *   - futex 字是用户空间一个 32 位整型（位于当前任务的地址空间内）。
 *   - 等待：调用方先确认 *uaddr == val，再把当前任务挂入「按 (cr3,uaddr) 哈希」
 *     的等待桶并 BLOCKED，随后 schedule() 让出 CPU；被 futex_wake 唤醒后摘除节点。
 *   - 唤醒：匹配 (cr3,uaddr) 的等待桶中最多 n 个等待者，逐个 sched_wake。
 *   - 哈希键含 cr3：同一 cr3（线程/COW 子进程）视为同一地址空间；不同进程即便
 *     虚拟地址相同也互不干扰（进程间共享内存的 futex 属后续增强，本实现足以支撑
 *     线程同步：线程共享 cr3，天然落入同一桶）。
 *
 * 竞态防护：插入等待桶【与】二次校验 *uaddr==val 都在 g_futex_lock 下完成，且置
 * BLOCKED 也在同一持锁区间内——故不会与 futex_wake 的「改值 + 唤醒」丢失配对，
 * 不存在「唤醒丢失导致永久睡眠」。
 *
 * 实现见 kernel/sched/futex.c。
 */
#ifndef _SUKI_KERNEL_FUTEX_H
#define _SUKI_KERNEL_FUTEX_H

#include <kernel/types.h>

/*
 * 阻塞等待：仅当 *uaddr == val 时才睡眠，否则立即返回 -EAGAIN。
 * timeout_ns 当前忽略（始终阻塞至被唤醒）。
 * 返回 0 表示被成功唤醒；<0 为负 errno（-EAGAIN 值不匹配 / -EFAULT 地址非法）。
 */
int futex_wait(uint32_t *uaddr, uint32_t val, uint64_t timeout_ns);

/*
 * 唤醒：最多唤醒 n 个（n<=0 表示全部）匹配 (cr3,uaddr) 的等待者。
 * 返回实际唤醒的数量。
 */
int futex_wake(uint32_t *uaddr, int n);

#endif /* _SUKI_KERNEL_FUTEX_H */
