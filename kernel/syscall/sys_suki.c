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
#include <mm/kmalloc.h>
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

void suki_obj_unref(suki_object_t *o)
{
    if (!o)
        return;
    uint64_t f = spin_lock_irqsave(&g_suki_lock);
    uint32_t rc = --o->refcount;
    spin_unlock_irqrestore(&g_suki_lock, f);
    if (rc == 0)
        kfree(o);
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
    /* Phase 2（按需）：FILE/PROC/MEM 对象 */
    case SYS_SUKI_FILE_OPEN:
    case SYS_SUKI_FILE_READ:
    case SYS_SUKI_FILE_WRITE:
    case SYS_SUKI_FILE_CLOSE:
    case SYS_SUKI_PROC_CREATE:
    case SYS_SUKI_MEM_ALLOC:
        return (uint64_t)-SUKI_ENOSYS;

    default:
        return (uint64_t)-SUKI_ENOSYS;
    }
}
