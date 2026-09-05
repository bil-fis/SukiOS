/*
 * kernel/syscall/sys_suki.c — SukiNative 原生对象 API 分发与实现（130..149）
 *
 * Phase 1 交付范围：对象/句柄子系统 + 事件/互斥/信号量 + SYS_SUKI_WAIT 多对象等待。
 * 设计要点见 include/kernel/suki_native.h。
 *
 * 号段（posix.h）：
 *   130 OBJ_CREATE  131 OBJ_DESTROY  132 OBJ_DUPLICATE  133 OBJ_QUERY
 *   134 WAIT        135 EVENT_CREATE 136 EVENT_SET      137 EVENT_RESET
 *   138 MUTEX_CREATE 139 MUTEX_LOCK  140 MUTEX_UNLOCK
 *   141 SEM_CREATE  142 SEM_ACQUIRE  143 SEM_RELEASE
 *   144..149 FILE/PROC/MEM 对象 —— 留待 Phase 2（按需），本阶段返回 -ENOSYS。
 *
 * 从用户态接收的指针（handles 数组、out_index、info）一律经
 * copy_from_user/copy_to_user 访问（项目强制规则）。
 */

#include <kernel/syscall.h>
#include <kernel/task.h>
#include <kernel/suki_native.h>
#include <kernel/spinlock.h>
#include <kernel/fd.h>
#include <mm/kmalloc.h>
#include <mm/vma.h>
#include <mm/vmm.h>
#include <kernel/serial.h>
#include <stdbool.h>
#include <sukios/posix.h>

static spinlock_t g_suki_lock = SPINLOCK_INIT("suki");

/* ===================== 对象生命周期 ===================== */

suki_object_t *suki_obj_new(suki_obj_type_t type)
{
    suki_object_t *o = (suki_object_t *)kzalloc(sizeof(suki_object_t));
    if (!o)
        return NULL;
    o->type     = type;
    o->refcount = 1;        /* 初引用由即将创建的句柄持有 */
    o->waiters  = NULL;
    return o;
}

void suki_obj_ref(suki_object_t *o)
{
    if (!o)
        return;
    uint64_t f = spin_lock_irqsave(&g_suki_lock);
    o->refcount++;
    spin_unlock_irqrestore(&g_suki_lock, f);
}

/* 前向声明（定义见下方 suki_obj_unref 之后） */
static void suki_obj_free(suki_object_t *o);
static void suki_proc_detach(suki_object_t *o);

void suki_obj_unref(suki_object_t *o)
{
    if (!o)
        return;
    uint64_t f = spin_lock_irqsave(&g_suki_lock);
    uint32_t rc = --o->refcount;
    spin_unlock_irqrestore(&g_suki_lock, f);
    if (rc == 0) {
        suki_obj_free(o);   /* 类型特定清理（fd_close/vma_unmap 可能阻塞，必须在锁外） */
        kfree(o);
    }
}

/* 从子任务 suki_exit_notify 链摘除本 PROC 对象。若子进程已退出，
 * suki_proc_notify_exit 已把 o->next_notify 置 NULL 表示脱离，此时直接返回。 */
static void suki_proc_detach(suki_object_t *o)
{
    task_t *child = o->u.proc.child;
    if (!child || o->next_notify == NULL)
        return;
    uint64_t f = spin_lock_irqsave(&g_suki_lock);
    suki_object_t **pp = &child->suki_exit_notify;
    while (*pp) {
        if (*pp == o) {
            *pp = o->next_notify;
            o->next_notify = NULL;
            break;
        }
        pp = &(*pp)->next_notify;
    }
    spin_unlock_irqrestore(&g_suki_lock, f);
}

/* 对象引用归零时的类型特定清理。调用时对象已无其它引用，但 owner 可能已退出
 * （任务回收路径），需相应防御：FILE 的 fd 由 suki_owned 标记保证 fd_exit_task 跳过，
 * 此处 fd_close 仍可安全释放；MEM 的 owner 已死时地址空间已销毁，跳过 unmap；
 * PROC 需从子任务 notify 链摘除，避免悬挂指针。 */
static void suki_obj_free(suki_object_t *o)
{
    switch (o->type) {
    case SUKI_OT_FILE:
        if (o->u.file.owner && o->u.file.fd >= 0)
            fd_close(o->u.file.owner, o->u.file.fd);
        break;
    case SUKI_OT_MEM:
        if (o->u.mem.owner && !o->u.mem.owner->dead && o->u.mem.uaddr)
            vma_unmap_range(o->u.mem.owner, o->u.mem.uaddr,
                            o->u.mem.uaddr + o->u.mem.size);
        break;
    case SUKI_OT_PROC:
        suki_proc_detach(o);
        break;
    default:
        break;
    }
}

/* ===================== 句柄表（惰性分配） ===================== */

/* 首次使用时为当前任务分配句柄表；kzalloc 零初始化，所有项 occupied=false。 */
static int handles_init(task_t *cur)
{
    if (cur->suki_handles)
        return 0;
    cur->suki_handles = (suki_handle_entry_t *)kzalloc(
        sizeof(suki_handle_entry_t) * SUKI_MAX_HANDLES);
    if (!cur->suki_handles)
        return -SUKI_ENOMEM;
    cur->suki_handle_cap = SUKI_MAX_HANDLES;
    return 0;
}

int suki_handle_alloc(suki_object_t *o, uint32_t rights, suki_handle_t *out)
{
    task_t *cur = sched_current();
    if (handles_init(cur) != 0)
        return -SUKI_ENOMEM;
    for (uint32_t i = 0; i < cur->suki_handle_cap; i++) {
        if (!cur->suki_handles[i].occupied) {
            cur->suki_handles[i].occupied = true;
            cur->suki_handles[i].obj      = o;     /* 持有 o 的初始引用（不另增） */
            cur->suki_handles[i].rights   = rights;
            *out = i + 1;                          /* 句柄 0 保留为非法 */
            return 0;
        }
    }
    return -SUKI_EMFILE;
}

int suki_handle_lookup(suki_handle_t h, suki_object_t **out)
{
    task_t *cur = sched_current();
    if (!cur->suki_handles)
        return -SUKI_EBADF;
    if (h == 0 || (h - 1) >= cur->suki_handle_cap)
        return -SUKI_EBADF;
    suki_handle_entry_t *e = &cur->suki_handles[h - 1];
    if (!e->occupied)
        return -SUKI_EBADF;
    *out = e->obj;
    return 0;
}

int suki_handle_free(suki_handle_t h)
{
    task_t *cur = sched_current();
    if (!cur->suki_handles)
        return -SUKI_EBADF;
    if (h == 0 || (h - 1) >= cur->suki_handle_cap)
        return -SUKI_EBADF;
    suki_handle_entry_t *e = &cur->suki_handles[h - 1];
    if (!e->occupied)
        return -SUKI_EBADF;
    suki_object_t *o = e->obj;
    e->occupied = false;
    e->obj      = NULL;
    e->rights   = 0;
    suki_obj_unref(o);          /* 释放句柄引用；引用归零后对象销毁 */
    return 0;
}

void suki_handles_free_all(struct task *t)
{
    if (!t->suki_handles)
        return;
    /* 目标任务已 dead（不运行），其句柄表无并发访问；逐个 unref（内部持 g_suki_lock
     * 为引用计数提供原子性）。 */
    for (uint32_t i = 0; i < t->suki_handle_cap; i++) {
        if (t->suki_handles[i].occupied) {
            suki_object_t *o = t->suki_handles[i].obj;
            t->suki_handles[i].occupied = false;
            t->suki_handles[i].obj      = NULL;
            suki_obj_unref(o);
        }
    }
    kfree(t->suki_handles);
    t->suki_handles    = NULL;
    t->suki_handle_cap = 0;
}

/* ===================== 等待语义 ===================== */

bool suki_obj_ready(suki_object_t *o)
{
    switch (o->type) {
    case SUKI_OT_EVENT: return o->u.event.signaled;
    case SUKI_OT_MUTEX: return !o->u.mutex.locked;
    case SUKI_OT_SEM:   return o->u.sem.count > 0;
    case SUKI_OT_PROC:  return o->u.proc.exited;   /* 子进程已退出即就绪 */
    default:            return false;
    }
}

void suki_obj_consume(suki_object_t *o)
{
    switch (o->type) {
    case SUKI_OT_EVENT:
        if (!o->u.event.manual_reset)
            o->u.event.signaled = false;   /* auto-reset：消费一次即复位 */
        break;
    case SUKI_OT_MUTEX:
        o->u.mutex.locked = true;
        o->u.mutex.owner  = sched_current();
        break;
    case SUKI_OT_SEM:
        if (o->u.sem.count > 0)
            o->u.sem.count--;
        break;
    case SUKI_OT_PROC:
        /* 子进程退出态保持直到对象销毁，供重复查询，不消费 */
        break;
    default:
        break;
    }
}

void suki_obj_wake_all(suki_object_t *o)
{
    task_t *list[64];
    int n = 0;
    uint64_t f = spin_lock_irqsave(&g_suki_lock);
    suki_waitnode_t *w = o->waiters;
    while (w) {
        if (n < 64)
            list[n++] = w->task;
        w = w->next;
    }
    o->waiters = NULL;   /* 唤醒本对象全部等待者；多对象等待由等待方自身在唤醒后清理 */
    spin_unlock_irqrestore(&g_suki_lock, f);
    for (int i = 0; i < n; i++)
        sched_wake(list[i]);
}

/*
 * 核心多对象等待：objs 已解析，flags 见 SUKI_WAIT_*，命中写 out_index。
 * 返回 0 命中；-EAGAIN 非阻塞且无就绪；其它为负 errno。
 * 协议（防丢失唤醒，对照 futex）：就绪检查与入 waiter 链表均在持 g_suki_lock 期间
 * 完成，随后才释放锁并 schedule()；信号方持锁设置状态后调 suki_obj_wake_all。
 */
static int suki_wait_objects(suki_object_t **objs, int n, uint32_t flags, size_t *out_index)
{
    task_t *cur = sched_current();
    uint64_t f = spin_lock_irqsave(&g_suki_lock);
    for (;;) {
        int found = -1;
        bool all_ok = true;
        for (int i = 0; i < n; i++) {
            if (suki_obj_ready(objs[i])) {
                found = i;
                if (!(flags & SUKI_WAIT_ALL))
                    break;
            } else if (flags & SUKI_WAIT_ALL) {
                all_ok = false;
            }
        }
        if (flags & SUKI_WAIT_ALL && !all_ok)
            found = -1;

        if (found >= 0) {
            if (flags & SUKI_WAIT_ALL)
                for (int i = 0; i < n; i++)
                    suki_obj_consume(objs[i]);
            else
                suki_obj_consume(objs[found]);
            spin_unlock_irqrestore(&g_suki_lock, f);
            *out_index = (size_t)found;
            return 0;
        }

        if (flags & SUKI_WAIT_NO_BLOCK) {
            spin_unlock_irqrestore(&g_suki_lock, f);
            return -SUKI_EAGAIN;
        }

        /* 阻塞：把本任务挂入每个对象的 waiter 链表（每对象一个节点） */
        for (int i = 0; i < n; i++) {
            cur->suki_wait_nodes[i].task = cur;
            cur->suki_wait_nodes[i].next = objs[i]->waiters;
            objs[i]->waiters = &cur->suki_wait_nodes[i];
            cur->suki_wait_set[i] = objs[i];
        }
        cur->suki_wait_n      = n;
        cur->suki_wait_active = true;
        cur->state = BLOCKED;
        cur->in_rq = false;
        spin_unlock_irqrestore(&g_suki_lock, f);

        schedule();   /* 被 suki_obj_wake_all 经 sched_wake 唤醒后从此返回 */

        /* 唤醒：从所有等待对象链表中摘除本任务节点 */
        f = spin_lock_irqsave(&g_suki_lock);
        for (int i = 0; i < cur->suki_wait_n; i++) {
            suki_waitnode_t **pp = &cur->suki_wait_set[i]->waiters;
            while (*pp) {
                if (*pp == &cur->suki_wait_nodes[i]) {
                    *pp = cur->suki_wait_nodes[i].next;
                    break;
                }
                pp = &(*pp)->next;
            }
        }
        cur->suki_wait_active = false;
        cur->suki_wait_n      = 0;
        /* 继续 for(;;) 重新评估就绪态（应对虚假唤醒/多任务竞争） */
    }
}

/* ===================== 分发 ===================== */

/* Phase 2 新增 handler 前向声明（定义见文件末尾） */
static uint64_t sys_suki_file_open(task_t *t, uint64_t a1, uint64_t a2,
                                    uint64_t a3, suki_handle_t *out);
static uint64_t sys_suki_file_read(task_t *t, uint64_t a1, uint64_t a2,
                                   uint64_t a3, size_t *out_n);
static uint64_t sys_suki_file_write(task_t *t, uint64_t a1, uint64_t a2,
                                    uint64_t a3, size_t *out_n);
static uint64_t sys_suki_file_close(task_t *t, uint64_t a1);
static uint64_t sys_suki_proc_create(task_t *t, uint64_t a1, uint64_t a2,
                                     uint64_t a3, suki_handle_t *out);
static uint64_t sys_suki_mem_alloc(task_t *t, uint64_t a1, suki_handle_t *out);

uint64_t sys_suki_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a6;

    switch (num) {
    case SYS_SUKI_OBJ_CREATE: {       /* type=a1, a2/a3 类型相关 */
        suki_obj_type_t t = (suki_obj_type_t)a1;
        if (t < SUKI_OT_EVENT || t > SUKI_OT_MEM)
            return (uint64_t)-SUKI_EINVAL;
        suki_object_t *o = suki_obj_new(t);
        if (!o)
            return (uint64_t)-SUKI_ENOMEM;
        switch (t) {
        case SUKI_OT_EVENT:
            o->u.event.signaled     = (bool)a2;
            o->u.event.manual_reset = (bool)a3;
            break;
        case SUKI_OT_MUTEX:
            o->u.mutex.locked = false; o->u.mutex.owner = NULL;
            break;
        case SUKI_OT_SEM:
            o->u.sem.count = (int)a2; o->u.sem.max = (int)a3;
            break;
        default:
            break;
        }
        suki_handle_t h;
        if (suki_handle_alloc(o, 0, &h) != 0) {
            suki_obj_unref(o);
            return (uint64_t)-SUKI_ENOMEM;
        }
        return (uint64_t)h;
    }
    case SYS_SUKI_OBJ_DESTROY: {      /* handle=a1 */
        if (suki_handle_free((suki_handle_t)a1) != 0)
            return (uint64_t)-SUKI_EBADF;
        return 0;
    }
    case SYS_SUKI_OBJ_DUPLICATE: {    /* handle=a1, rights=a2 */
        suki_object_t *o;
        if (suki_handle_lookup((suki_handle_t)a1, &o) != 0)
            return (uint64_t)-SUKI_EBADF;
        suki_handle_t h;
        if (suki_handle_alloc(o, (uint32_t)a2, &h) != 0)
            return (uint64_t)-SUKI_EMFILE;
        suki_obj_ref(o);              /* 第二个句柄持有一次引用 */
        return (uint64_t)h;
    }
    case SYS_SUKI_OBJ_QUERY: {        /* handle=a1, info_user=a2 */
        suki_object_t *o;
        if (suki_handle_lookup((suki_handle_t)a1, &o) != 0)
            return (uint64_t)-SUKI_EBADF;
        suki_objinfo_t info;
        info.type  = (uint32_t)o->type;
        info.state = suki_obj_ready(o) ? 1u : 0u;
        info.count = (int32_t)(o->type == SUKI_OT_SEM ? o->u.sem.count : 0);
        info._pad  = 0;
        info.base  = (o->type == SUKI_OT_MEM) ? o->u.mem.uaddr
                   : (o->type == SUKI_OT_PROC) ? o->u.proc.pid : 0;
        info.size  = (o->type == SUKI_OT_MEM) ? o->u.mem.size : 0;
        if (copy_to_user((void *)a2, &info, sizeof(info)) != sizeof(info))
            return (uint64_t)-SUKI_EFAULT;
        return 0;
    }
    case SYS_SUKI_WAIT: {             /* handles_user=a1, count=a2, flags=a3,
                                         timeout=a4(暂未用), out_index_user=a5 */
        size_t count = (size_t)a2;
        if (count == 0 || count > SUKI_MAX_WAIT)
            return (uint64_t)-SUKI_EINVAL;
        if (!a1)
            return (uint64_t)-SUKI_EFAULT;
        suki_handle_t hbuf[SUKI_MAX_WAIT];
        if (copy_from_user(hbuf, (const void *)a1,
                            count * sizeof(suki_handle_t)) !=
            count * sizeof(suki_handle_t))
            return (uint64_t)-SUKI_EFAULT;
        suki_object_t *objs[SUKI_MAX_WAIT];
        for (size_t i = 0; i < count; i++) {
            if (suki_handle_lookup(hbuf[i], &objs[i]) != 0)
                return (uint64_t)-SUKI_EBADF;
        }
        size_t idx = 0;
        int r = suki_wait_objects(objs, (int)count, (uint32_t)a3, &idx);
        if (r != 0)
            return (uint64_t)(int64_t)r;
        if (copy_to_user((void *)a5, &idx, sizeof(idx)) != sizeof(idx))
            return (uint64_t)-SUKI_EFAULT;
        return 0;
    }
    case SYS_SUKI_EVENT_CREATE: {     /* init_signaled=a1, manual_reset=a2 */
        suki_object_t *o = suki_obj_new(SUKI_OT_EVENT);
        if (!o)
            return (uint64_t)-SUKI_ENOMEM;
        o->u.event.signaled     = (bool)a1;
        o->u.event.manual_reset = (bool)a2;
        suki_handle_t h;
        if (suki_handle_alloc(o, 0, &h) != 0) {
            suki_obj_unref(o);
            return (uint64_t)-SUKI_ENOMEM;
        }
        return (uint64_t)h;
    }
    case SYS_SUKI_EVENT_SET: {        /* handle=a1 */
        suki_object_t *o;
        if (suki_handle_lookup((suki_handle_t)a1, &o) != 0)
            return (uint64_t)-SUKI_EBADF;
        if (o->type != SUKI_OT_EVENT)
            return (uint64_t)-SUKI_EINVAL;
        uint64_t f = spin_lock_irqsave(&g_suki_lock);
        o->u.event.signaled = true;
        spin_unlock_irqrestore(&g_suki_lock, f);
        suki_obj_wake_all(o);
        return 0;
    }
    case SYS_SUKI_EVENT_RESET: {      /* handle=a1 */
        suki_object_t *o;
        if (suki_handle_lookup((suki_handle_t)a1, &o) != 0)
            return (uint64_t)-SUKI_EBADF;
        if (o->type != SUKI_OT_EVENT)
            return (uint64_t)-SUKI_EINVAL;
        uint64_t f = spin_lock_irqsave(&g_suki_lock);
        o->u.event.signaled = false;
        spin_unlock_irqrestore(&g_suki_lock, f);
        return 0;
    }
    case SYS_SUKI_MUTEX_CREATE: {     /* 无参 */
        suki_object_t *o = suki_obj_new(SUKI_OT_MUTEX);
        if (!o)
            return (uint64_t)-SUKI_ENOMEM;
        o->u.mutex.locked = false; o->u.mutex.owner = NULL;
        suki_handle_t h;
        if (suki_handle_alloc(o, 0, &h) != 0) {
            suki_obj_unref(o);
            return (uint64_t)-SUKI_ENOMEM;
        }
        return (uint64_t)h;
    }
    case SYS_SUKI_MUTEX_LOCK: {       /* handle=a1 */
        suki_object_t *o;
        if (suki_handle_lookup((suki_handle_t)a1, &o) != 0)
            return (uint64_t)-SUKI_EBADF;
        if (o->type != SUKI_OT_MUTEX)
            return (uint64_t)-SUKI_EINVAL;
        if (o->u.mutex.locked && o->u.mutex.owner == sched_current())
            return (uint64_t)-SUKI_EDEADLK;   /* 自锁检测，避免单任务测试挂死 */
        size_t idx = 0;
        int r = suki_wait_objects(&o, 1, 0, &idx);   /* 锁空闲即就绪，否则阻塞 */
        return (uint64_t)(int64_t)r;
    }
    case SYS_SUKI_MUTEX_UNLOCK: {     /* handle=a1 */
        suki_object_t *o;
        if (suki_handle_lookup((suki_handle_t)a1, &o) != 0)
            return (uint64_t)-SUKI_EBADF;
        if (o->type != SUKI_OT_MUTEX)
            return (uint64_t)-SUKI_EINVAL;
        uint64_t f = spin_lock_irqsave(&g_suki_lock);
        o->u.mutex.locked = false;
        o->u.mutex.owner  = NULL;
        spin_unlock_irqrestore(&g_suki_lock, f);
        suki_obj_wake_all(o);
        return 0;
    }
    case SYS_SUKI_SEM_CREATE: {       /* initial=a1, max=a2 */
        suki_object_t *o = suki_obj_new(SUKI_OT_SEM);
        if (!o)
            return (uint64_t)-SUKI_ENOMEM;
        o->u.sem.count = (int)a1; o->u.sem.max = (int)a2;
        suki_handle_t h;
        if (suki_handle_alloc(o, 0, &h) != 0) {
            suki_obj_unref(o);
            return (uint64_t)-SUKI_ENOMEM;
        }
        return (uint64_t)h;
    }
    case SYS_SUKI_SEM_ACQUIRE: {      /* handle=a1 */
        suki_object_t *o;
        if (suki_handle_lookup((suki_handle_t)a1, &o) != 0)
            return (uint64_t)-SUKI_EBADF;
        if (o->type != SUKI_OT_SEM)
            return (uint64_t)-SUKI_EINVAL;
        size_t idx = 0;
        int r = suki_wait_objects(&o, 1, 0, &idx);   /* 计数>0 即就绪 */
        return (uint64_t)(int64_t)r;
    }
    case SYS_SUKI_SEM_RELEASE: {      /* handle=a1 */
        suki_object_t *o;
        if (suki_handle_lookup((suki_handle_t)a1, &o) != 0)
            return (uint64_t)-SUKI_EBADF;
        if (o->type != SUKI_OT_SEM)
            return (uint64_t)-SUKI_EINVAL;
        uint64_t f = spin_lock_irqsave(&g_suki_lock);
        if (o->u.sem.count < o->u.sem.max)
            o->u.sem.count++;
        spin_unlock_irqrestore(&g_suki_lock, f);
        suki_obj_wake_all(o);
        return 0;
    }
    /* Phase 2：FILE/PROC/MEM 对象（详见文件末尾实现） */
    case SYS_SUKI_FILE_OPEN:   return sys_suki_file_open(sched_current(), a1, a2, a3, (suki_handle_t*)a4);
    case SYS_SUKI_FILE_READ:   return sys_suki_file_read(sched_current(), a1, a2, a3, (size_t*)a4);
    case SYS_SUKI_FILE_WRITE:  return sys_suki_file_write(sched_current(), a1, a2, a3, (size_t*)a4);
    case SYS_SUKI_FILE_CLOSE:  return sys_suki_file_close(sched_current(), a1);
    case SYS_SUKI_PROC_CREATE: return sys_suki_proc_create(sched_current(), a1, a2, a3, (suki_handle_t*)a4);
    case SYS_SUKI_MEM_ALLOC:   return sys_suki_mem_alloc(sched_current(), a1, (suki_handle_t*)a2);

    default:
        return (uint64_t)-SUKI_ENOSYS;
    }
}

/* ===================== Phase 2 handler 实现（FILE/PROC/MEM 对象） ===================== */

/* FILE：封装 per-task fd（suki_owned 标记使 fd_exit_task 跳过，由对象销毁统一关闭）。 */
static uint64_t sys_suki_file_open(task_t *t, uint64_t a1_path, uint64_t a2_flags,
                                    uint64_t a3_mode, suki_handle_t *out_handle)
{
    if (!a1_path)
        return (uint64_t)-SUKI_EFAULT;
    char path_buf[512];
    int64_t plen = copy_str_from_user(path_buf, (const char *)a1_path, sizeof(path_buf));
    if (plen < 0)
        return (uint64_t)-SUKI_EFAULT;
    (void)out_handle;
    int fd = fd_open(t, path_buf, (int)a2_flags, (uint32_t)a3_mode);
    if (fd < 0)
        return (uint64_t)fd;            /* 负 errno */
    fd_entry_t *e = fd_get(t, fd);
    if (e) e->suki_owned = true;         /* 转由对象生命周期管理，避免双重关闭 */
    suki_object_t *o = suki_obj_new(SUKI_OT_FILE);
    if (!o) {
        fd_close(t, fd);
        return (uint64_t)-SUKI_ENOMEM;
    }
    o->u.file.fd    = fd;
    o->u.file.owner = t;
    suki_handle_t h;
    int r = suki_handle_alloc(o, 0, &h);
    if (r != 0) {
        suki_obj_unref(o);              /* 句柄分配失败：释放未挂链对象 */
        fd_close(t, fd);
        return (uint64_t)-SUKI_EMFILE;
    }
    return (uint64_t)h;                  /* 句柄持有初始引用，无需 unref（与 Phase 1 一致） */                  /* 句柄经 %rax 返回（与 Phase 1 一致） */
}

static uint64_t sys_suki_file_read(task_t *t, uint64_t a1_handle, uint64_t a2_ubuf,
                                   uint64_t a3_count, size_t *out_n)
{
    suki_object_t *o;
    if (suki_handle_lookup((suki_handle_t)a1_handle, &o) != 0)
        return (uint64_t)-SUKI_EBADF;
    if (o->type != SUKI_OT_FILE)
        return (uint64_t)-SUKI_EBADF;
    int fd = o->u.file.fd;
    if (fd < 0)
        return (uint64_t)-SUKI_EBADF;
    if (!a2_ubuf)
        return (uint64_t)-SUKI_EFAULT;
    suki_ssize_t n = fd_read(t, fd, (void *)a2_ubuf, a3_count);
    if (n < 0)
        return (uint64_t)n;
    size_t got = (size_t)n;
    if (out_n && copy_to_user(out_n, &got, sizeof(got)) != sizeof(got))
        return (uint64_t)-SUKI_EFAULT;
    return 0;
}

static uint64_t sys_suki_file_write(task_t *t, uint64_t a1_handle, uint64_t a2_ubuf,
                                    uint64_t a3_count, size_t *out_n)
{
    suki_object_t *o;
    if (suki_handle_lookup((suki_handle_t)a1_handle, &o) != 0)
        return (uint64_t)-SUKI_EBADF;
    if (o->type != SUKI_OT_FILE)
        return (uint64_t)-SUKI_EBADF;
    int fd = o->u.file.fd;
    if (fd < 0)
        return (uint64_t)-SUKI_EBADF;
    if (!a2_ubuf)
        return (uint64_t)-SUKI_EFAULT;
    suki_ssize_t n = fd_write(t, fd, (const void *)a2_ubuf, a3_count);
    if (n < 0)
        return (uint64_t)n;
    size_t got = (size_t)n;
    if (out_n && copy_to_user(out_n, &got, sizeof(got)) != sizeof(got))
        return (uint64_t)-SUKI_EFAULT;
    return 0;
}

static uint64_t sys_suki_file_close(task_t *t, uint64_t a1_handle)
{
    suki_object_t *o;
    if (suki_handle_lookup((suki_handle_t)a1_handle, &o) != 0)
        return (uint64_t)-SUKI_EBADF;
    if (o->type != SUKI_OT_FILE)
        return (uint64_t)-SUKI_EBADF;
    int fd = o->u.file.fd;
    o->u.file.fd = -1;                   /* 防对象销毁时重复关闭 */
    suki_handle_free((suki_handle_t)a1_handle);
    if (fd >= 0)
        fd_close(t, fd);
    return 0;
}

/* PROC：创建子进程并返回监控句柄；子进程退出时经 suki_proc_notify_exit 通知。 */
static uint64_t sys_suki_proc_create(task_t *t, uint64_t a1_path, uint64_t a2_argv,
                                     uint64_t a3_argc, suki_handle_t *out_handle)
{
    if (!a1_path)
        return (uint64_t)-SUKI_EFAULT;
    char path_buf[512];
    int64_t plen = copy_str_from_user(path_buf, (const char *)a1_path, sizeof(path_buf));
    if (plen < 0)
        return (uint64_t)-SUKI_EFAULT;
    (void)out_handle;

    /* 读取 ELF 文件内容到内核缓冲（循环 fd_read_kern 至 EOF，上限 8MB）。
     * 必须用 fd_read_kern（直接 memcpy 进内核 blob），不能用 fd_read——
     * fd_read 经 copy_to_user 写回【用户虚拟地址】，读进内核 blob 会 -EFAULT。 */
    int fd = fd_open(t, path_buf, SUKI_O_RDONLY, 0);
    if (fd < 0)
        return (uint64_t)fd;
    uint8_t *blob = (uint8_t *)kzalloc(8 * 1024 * 1024);
    if (!blob) {
        fd_close(t, fd);
        return (uint64_t)-SUKI_ENOMEM;
    }
    size_t total = 0;
    uint8_t *p = blob;
    while (total < 8 * 1024 * 1024) {
        suki_ssize_t n = fd_read_kern(t, fd, p, 8 * 1024 * 1024 - total);
        if (n <= 0)
            break;
        total += (size_t)n;
        p += n;
    }
    fd_close(t, fd);

    suki_object_t *o = suki_obj_new(SUKI_OT_PROC);
    if (!o) {
        kfree(blob);
        return (uint64_t)-SUKI_ENOMEM;
    }
    /* 创建子进程（elf_load 内部安全拷贝 argv 用户指针） */
    task_t *child = task_create_user_args(blob, total,
                                          (int)a3_argc, (const char **)a2_argv,
                                          0, NULL, path_buf);
    kfree(blob);
    if (!child) {
        suki_obj_unref(o);              /* 未挂链，直接释放 */
        return (uint64_t)-SUKI_EINVAL;  /* ELF 无效或资源不足 */
    }
    o->u.proc.child      = child;
    o->u.proc.pid        = child->id;
    o->u.proc.exited     = false;
    o->u.proc.exit_code  = 0;
    o->next_notify       = NULL;

    suki_handle_t h;
    int r = suki_handle_alloc(o, 0, &h);
    if (r != 0) {
        /* 句柄分配失败未挂链：释放对象（子进程已创建 -> 成为孤儿继续运行，可接受） */
        suki_obj_unref(o);
        return (uint64_t)-SUKI_EMFILE;
    }
    /* 挂入子任务退出通知链（持 g_suki_lock 串行化，避免与 suki_proc_notify_exit 竞争）。
     * 竞态兜底：若 child 在我们挂链前已退出，立即标记 exited 以便后续 wait 命中。 */
    uint64_t f = spin_lock_irqsave(&g_suki_lock);
    o->next_notify          = child->suki_exit_notify;
    child->suki_exit_notify = o;
    if (child->zombie || child->dead)
        o->u.proc.exited = true;
    spin_unlock_irqrestore(&g_suki_lock, f);

    return (uint64_t)h;                  /* 句柄经 %rax 返回 */
}

/* MEM：映射到本任务用户地址空间的匿名内存（按需零填充，与 sys_mmap 同机制）。 */
static uint64_t sys_suki_mem_alloc(task_t *t, uint64_t a1_size, suki_handle_t *out_handle)
{
    (void)out_handle;
    uint64_t size = a1_size;
    if (size == 0 || size > VMA_MMAP_MAX)
        return (uint64_t)-SUKI_EINVAL;
    uint64_t aligned = (size + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);
    uint64_t uaddr = vma_find_free(t, VMA_MMAP_BASE, VMA_MMAP_TOP, aligned);
    if (uaddr == 0)
        return (uint64_t)-SUKI_ENOMEM;
    suki_object_t *o = suki_obj_new(SUKI_OT_MEM);
    if (!o)
        return (uint64_t)-SUKI_ENOMEM;
    o->u.mem.uaddr = uaddr;
    o->u.mem.size  = aligned;
    o->u.mem.owner = t;
    uint64_t vprot = PTE_NX | PTE_WRITE;   /* RW 用户页（W^X：不可执行） */
    if (!vma_insert(t, uaddr, uaddr + aligned, vprot, VMA_TYPE_ANON)) {
        suki_obj_unref(o);
        return (uint64_t)-SUKI_ENOMEM;
    }
    suki_handle_t h;
    int r = suki_handle_alloc(o, 0, &h);
    if (r != 0) {
        suki_obj_unref(o);              /* 句柄分配失败：释放未挂链对象 */
        vma_unmap_range(t, uaddr, uaddr + aligned);
        return (uint64_t)-SUKI_EMFILE;
    }
    return (uint64_t)h;                  /* 句柄经 %rax 返回 */
}

/* 子进程退出通知：task_exit_current 在释放 g_sched_lock 后调用。遍历 dead 任务的
 * suki_exit_notify 链，对每个 PROC 对象置 exited=true、脱离链表（next_notify=NULL）、
 * 收集其等待者并（释放 g_suki_lock 后）经 sched_wake 唤醒，使阻塞在 SYS_SUKI_WAIT
 * 上等待该进程退出的任务得以继续。 */
void suki_proc_notify_exit(task_t *dead)
{
    suki_object_t *o = dead->suki_exit_notify;
    while (o) {
        suki_object_t *on = o->next_notify;
        task_t *waiters[SUKI_MAX_WAIT];
        int n = 0;
        uint64_t f = spin_lock_irqsave(&g_suki_lock);
        o->u.proc.exited  = true;
        o->next_notify    = NULL;          /* 脱离 dead 的链表（dead 结构将释放） */
        suki_waitnode_t *w = o->waiters;
        while (w && n < SUKI_MAX_WAIT) {
            waiters[n++] = w->task;
            w = w->next;
        }
        o->waiters = NULL;
        spin_unlock_irqrestore(&g_suki_lock, f);
        for (int i = 0; i < n; i++)
            sched_wake(waiters[i]);
        o = on;
    }
}
