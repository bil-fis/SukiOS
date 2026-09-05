/*
 * include/kernel/suki_native.h
 * -----------------------------------------------------------------------------
 * SukiNative 原生对象/句柄子系统（内核侧，Phase 1）。
 *
 * 设计要点（与 SukiNative API 规范一致）：
 *   - 一切皆对象（suki_object_t），用户态拿到 suki_handle_t 句柄（类 Mach 能力），
 *     不直接操作内核指针；句柄 = 每任务句柄表索引 +1（0 保留为非法）。
 *   - 对象用引用计数（refcount）管理生命周期；句柄释放即 unref，引用归零后销毁。
 *   - 等待语义：event=signaled 即就绪、mutex=未锁定即就绪、sem=计数>0 即就绪；
 *     SYS_SUKI_WAIT 支持多对象 ANY/ALL/NO_BLOCK 等待，由调度器 sched_wake 唤醒。
 *
 * 锁序（关键，避免死锁）：
 *   - 本文件所有对象状态变更在 g_suki_lock 保护下进行；
 *   - suki_obj_wake_all 先持锁收集等待任务、释放锁后再调 sched_wake（绝不在持
 *     g_suki_lock 时调用 sched_wake，否则与 suki_handles_free_all 的形成锁序相反）。
 *   - suki_handles_free_all 仅在任务退出（reap）路径调用，目标任务已 dead（不运行），
 *     故其句柄表无并发访问；其内 suki_obj_unref 持 g_suki_lock 为引用计数提供原子性。
 *
 * 与 task.h 的关系：本头前向声明 struct task，不 include task.h，避免包含环；task.h
 * 在定义 task_t 前 include 本头，从而可使用 suki_handle_entry_t 等类型。
 */
#ifndef _KERNEL_SUKI_NATIVE_H
#define _KERNEL_SUKI_NATIVE_H

#include <kernel/types.h>
#include <stdbool.h>
#include <sukios/posix.h>   /* suki_obj_type_t / suki_handle_t / suki_objinfo_t / SUKI_* 标志 */

struct task;   /* 前向声明，避免与 task.h 形成包含环 */

#define SUKI_MAX_HANDLES  256
#define SUKI_MAX_WAIT     16

/* 等待节点：任务阻塞于 SYS_SUKI_WAIT 时，用 task_t::suki_wait_nodes[] 为【每个】
 * 等待对象各提供一个节点串入该对象的 waiter 链表（单 next 指针无法同时挂多条链）。 */
typedef struct suki_waitnode {
    struct task *task;
    struct suki_waitnode *next;   /* 串入某对象 waiter 链表 */
} suki_waitnode_t;

/* 内核对象（一切皆对象） */
typedef struct suki_object {
    suki_obj_type_t type;
    uint32_t        refcount;       /* 初值 1 = 由首个句柄持有；句柄释放即 unref */
    suki_waitnode_t *waiters;       /* 等待在本对象上的任务链表头 */
    union {
        struct { bool signaled; bool manual_reset; } event;
        struct { bool locked; struct task *owner; }  mutex;
        struct { int  count; int  max; }             sem;
    } u;
} suki_object_t;

/* 每任务句柄表项：句柄 = 索引+1，obj 指向内核对象（持有一次引用）。 */
typedef struct suki_handle_entry {
    suki_object_t *obj;
    uint32_t       rights;          /* 预留权限位（Phase 1 仅用 0=全权） */
    bool           occupied;
} suki_handle_entry_t;

/* 对象生命周期（refcount 1 = 初始引用，由首个句柄持有；句柄释放即 unref） */
suki_object_t *suki_obj_new(suki_obj_type_t type);
void suki_obj_ref(suki_object_t *o);
void suki_obj_unref(suki_object_t *o);

/* 句柄表：基于当前任务，惰性分配（首次 SukiNative 调用时）。 */
int  suki_handle_alloc(suki_object_t *o, uint32_t rights, suki_handle_t *out);
int  suki_handle_lookup(suki_handle_t h, suki_object_t **out);
int  suki_handle_free(suki_handle_t h);
void suki_handles_free_all(struct task *t);   /* 任务退出时回收全部句柄并 unref 对象 */

/* 等待语义 */
bool suki_obj_ready(suki_object_t *o);                 /* 对象是否就绪（可 wait 命中） */
void suki_obj_consume(suki_object_t *o);               /* 命中时消费就绪态（auto-reset/加锁/减计数） */
void suki_obj_wake_all(suki_object_t *o);              /* 唤醒等待本对象的所有任务 */

#endif /* _KERNEL_SUKI_NATIVE_H */
