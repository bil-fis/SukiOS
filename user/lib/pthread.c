/*
 * user/lib/pthread.c
 * -----------------------------------------------------------------------------
 * SukiOS 原生 pthread 实现（用户态线程库）。
 *
 * 设计（与内核 SYS_CLONE / SYS_FUTEX / SYS_ARCH_PRCTL 配合）：
 *   - 每线程 = 一个内核 task；pthread_create 经 SYS_CLONE(SHARE-VM) 创建，与父线程
 *     共享地址空间（cr3 相同）、VMA 与 fd 槽表；各自独立内核栈、独立用户栈、独立 TLS。
 *   - TLS：FS base 经 arch_prctl(ARCH_SET_FS) 设为指向线程控制块（TCB）；TCB 首 8
 *     字节为 self 指针，故 pthread_self() = *(pthread_t*)%fs:0。
 *   - 线程入口：clone 把子线程 RIP 直接设为 thread_trampoline，RSP 设为独立用户栈顶，
 *     故子线程不经过父的 clone 返回路径，启动即进入 trampoline -> start -> pthread_exit。
 *   - 同步：mutex/cond 基于内核 futex；join 在目标线程的 join_futex 字上等待
 *     （目标线程退出时置 tid 并 futex_wake；内核 clear_child_tid 额外清零并唤醒，供
 *     后续增强的 join 语义）。
 *
 * 目标：可稳定支撑多线程 Linux 程序移植（本阶段覆盖 create/join/exit/self + 互斥锁
 * + 条件变量 + once）。
 */
#include "libc.h"
#include "pthread.h"

/* ---- 线程控制块（TCB） ---- */
typedef struct pthread_tcb {
    struct pthread_tcb *self;     /* 必须位于偏移 0：%fs:0 即此 self 指针 */
    void *(*start)(void *);
    void *arg;
    void *retval;
    pid_t tid;
    int   joined;
    int   exit_flag;
    uint32_t join_futex;          /* join 等待字：线程退出时置为 tid */
    uint32_t clear_tid;           /* 内核退出清零字（独立于 join_futex） */
    void *stack;                  /* mmap 分配的栈基址（join 时释放） */
    size_t stack_size;
    struct pthread_tcb *next;     /* 全局线程链表链接 */
} pthread_tcb_t;

/* ---- 主线程 TLS 惰性初始化 ---- */
static pthread_tcb_t *g_main_tcb = NULL;
static int g_tls_inited = 0;

/* 读取当前线程 TCB（FS base 指向 TCB，首 8 字节为 self 指针） */
static inline pthread_tcb_t *tls_self(void)
{
    pthread_tcb_t *p;
    __asm__ __volatile__("movq %%fs:0, %0" : "=r"(p));
    return p;
}

/* 为主线程（进程 leader）建立 TLS（首次调用任意 pthread 函数前必须完成，
 * 且必须在创建任何工作线程【之前】，故 pthread_create 入口即调用本函数） */
static void pthread_ensure_main_tls(void)
{
    if (__atomic_load_n(&g_tls_inited, __ATOMIC_ACQUIRE))
        return;
    pthread_tcb_t *t = (pthread_tcb_t *)malloc(sizeof(*t));
    if (!t)
        return;
    memset(t, 0, sizeof(*t));
    t->self = t;
    t->tid  = gettid();
    arch_prctl(SUKI_ARCH_SET_FS, t);
    g_main_tcb = t;
    __atomic_store_n(&g_tls_inited, 1, __ATOMIC_RELEASE);
}

/* ---- futex 便捷封装（基于内核 SYS_FUTEX） ---- */
static inline int futex_wait_u32(uint32_t *uaddr, uint32_t val)
{
    return futex(uaddr, 0 /* FUTEX_WAIT */, val, NULL, NULL, 0);
}
static inline int futex_wake_u32(uint32_t *uaddr, int n)
{
    return futex(uaddr, 1 /* FUTEX_WAKE */, 0, NULL, NULL, 0);
}

/* ---- 线程入口（由内核 clone 直接跳入；RSP=独立用户栈顶，FS base=本线程 TCB） ---- */
static void thread_trampoline(void)
{
    pthread_tcb_t *t = tls_self();
    void *r = t->start(t->arg);
    pthread_exit(r);
}

/* ========================================================================== */
/*  线程生命周期                                                              */
/* ========================================================================== */

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg)
{
    (void)attr;
    if (!thread || !start_routine)
        return EINVAL;

    /* 必须最先为主线程建立 TLS，确保任何工作线程运行前 g_tls_inited 已置位 */
    pthread_ensure_main_tls();

    /* 分配独立用户栈（2MB，PROT_READ|WRITE，MAP_PRIVATE|ANON） */
    size_t sz = 2 * 1024 * 1024;
    void *stack = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED)
        return ENOMEM;

    /* TCB 置于栈区顶端：子线程 RSP 自 TCB 起向低地址增长，绝不踩踏 TCB */
    pthread_tcb_t *tcb = (pthread_tcb_t *)((char *)stack + sz - sizeof(pthread_tcb_t));
    memset(tcb, 0, sizeof(*tcb));
    tcb->self      = tcb;
    tcb->start     = start_routine;
    tcb->arg       = arg;
    tcb->stack     = stack;
    tcb->stack_size = sz;
    tcb->tid       = 0;
    tcb->join_futex = 0;
    tcb->clear_tid  = 0;

    void *child_stack_top = (void *)tcb;          /* RSP 起点 = TCB 地址 */
    void *tls             = (void *)tcb;          /* FS base = TCB 地址 */

    long tid = clone(SUKI_CLONE_VM | SUKI_CLONE_FILES | SUKI_CLONE_THREAD
                     | SUKI_CLONE_SETTLS | SUKI_CLONE_CHILD_SETTID
                     | SUKI_CLONE_CHILD_CLEARTID,
                     child_stack_top, (void *)thread_trampoline,
                     tls, &tcb->tid, &tcb->clear_tid);
    if (tid < 0) {
        munmap(stack, sz);
        return (int)(-tid);
    }

    *thread = tcb;
    return 0;
}

pthread_t pthread_self(void)
{
    if (!__atomic_load_n(&g_tls_inited, __ATOMIC_ACQUIRE))
        pthread_ensure_main_tls();
    return (pthread_t)tls_self();
}

int pthread_join(pthread_t thread, void **retval)
{
    if (!thread || thread->joined)
        return EINVAL;
    thread->joined = 1;

    /* 等待目标线程退出：其 pthread_exit 会把 join_futex 置为 tid 并 futex_wake */
    while (__atomic_load_n(&thread->join_futex, __ATOMIC_ACQUIRE) != thread->tid)
        futex_wait_u32(&thread->join_futex, 0);

    if (retval)
        *retval = thread->retval;

    /* 释放线程栈（含 TCB，TCB 位于栈区顶端） */
    munmap(thread->stack, thread->stack_size);
    return 0;
}

void pthread_exit(void *retval)
{
    pthread_tcb_t *t = tls_self();
    t->retval   = retval;
    t->exit_flag = 1;
    /* 通知 joiner：把 join_futex 置为 tid，并唤醒一个等待者 */
    __atomic_store_n(&t->join_futex, (uint32_t)t->tid, __ATOMIC_RELEASE);
    futex_wake_u32(&t->join_futex, 1);
    /* 退出当前线程（内核 clear_child_tid 会清零 clear_tid 并唤醒；地址空间由
     * 最后退出的线程负责释放） */
    suki_syscall1(SYS_TASK_EXIT, 0);
    __builtin_unreachable();
}

int pthread_detach(pthread_t thread)
{
    if (!thread)
        return EINVAL;
    thread->joined = 1;   /* 简化：标记已分离，join 将返回 EINVAL */
    return 0;
}

/* ========================================================================== */
/*  属性                                                                      */
/* ========================================================================== */

int pthread_attr_init(pthread_attr_t *attr)
{
    if (!attr) return EINVAL;
    attr->detached = 0;
    return 0;
}
int pthread_attr_destroy(pthread_attr_t *attr)
{
    (void)attr;
    return 0;
}
int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate)
{
    if (!attr) return EINVAL;
    attr->detached = detachstate;
    return 0;
}

/* ========================================================================== */
/*  互斥锁（futex 基座）                                                      */
/* ========================================================================== */

int pthread_mutex_init(pthread_mutex_t *mutex, const void *attr)
{
    (void)attr;
    if (!mutex) return EINVAL;
    *mutex = 0;
    return 0;
}
int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    (void)mutex;
    return 0;
}
int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
    if (!mutex) return EINVAL;
    /* CAS 0->1：成功返回 0，已被占用返回 EBUSY */
    return (__sync_val_compare_and_swap((int *)mutex, 0, 1) == 0) ? 0 : EBUSY;
}
int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    if (!mutex) return EINVAL;
    /* 自旋获取；若已被占用则在 futex 上等待直到被释放唤醒 */
    while (__sync_val_compare_and_swap((int *)mutex, 0, 1) != 0) {
        futex_wait_u32((uint32_t *)mutex, 1);
    }
    return 0;
}
int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    if (!mutex) return EINVAL;
    *mutex = 0;
    futex_wake_u32((uint32_t *)mutex, 1);
    return 0;
}

/* ========================================================================== */
/*  条件变量（seq 计数器 + futex）                                            */
/* ========================================================================== */

int pthread_cond_init(pthread_cond_t *cond, const void *attr)
{
    (void)attr;
    if (!cond) return EINVAL;
    *cond = 0;
    return 0;
}
int pthread_cond_destroy(pthread_cond_t *cond)
{
    (void)cond;
    return 0;
}
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    if (!cond || !mutex) return EINVAL;
    uint32_t seq = (uint32_t)*cond;
    pthread_mutex_unlock(mutex);             /* 释放锁，允许其它线程改变谓词并 signal */
    futex_wait_u32((uint32_t *)cond, seq);   /* 等待 seq 变化（signal 会自增 seq） */
    pthread_mutex_lock(mutex);               /* 重新持有锁后返回，谓词可安全重检 */
    return 0;
}
int pthread_cond_signal(pthread_cond_t *cond)
{
    if (!cond) return EINVAL;
    (*cond)++;                              /* 自增序列号，标记「状态已变」 */
    futex_wake_u32((uint32_t *)cond, 1);
    return 0;
}
int pthread_cond_broadcast(pthread_cond_t *cond)
{
    if (!cond) return EINVAL;
    (*cond)++;
    futex_wake_u32((uint32_t *)cond, 0x7fffffff);  /* 唤醒全部等待者 */
    return 0;
}

/* ========================================================================== */
/*  一次性初始化                                                              */
/* ========================================================================== */

int pthread_once(pthread_once_t *once_control, void (*init_routine)(void))
{
    if (!once_control || !init_routine)
        return EINVAL;
    /* 0 = 未执行；1 = 执行中/已完成。用访问即置位确保只执行一次 */
    if (__atomic_load_n(once_control, __ATOMIC_ACQUIRE) == 0) {
        if (__sync_bool_compare_and_swap(once_control, 0, 1)) {
            init_routine();
        }
    }
    return 0;
}
