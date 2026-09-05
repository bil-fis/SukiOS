/*
 * user/lib/pthread.h
 * -----------------------------------------------------------------------------
 * SukiOS 原生 pthread 接口（POSIX 线程子集，生产级可用）。
 *
 * 实现基座（用户态，见 pthread.c）：
 *   - 线程 = 内核 task 的一个实例，通过 SYS_CLONE(SHare-VM) 创建，与父线程共享
 *     同一地址空间（cr3 相同）、VMA 链表与 fd 槽表；各有独立内核栈、独立用户栈
 *     与独立 TLS（FS base 经 arch_prctl(ARCH_SET_FS) 设置）。
 *   - TLS 采用 x86_64 标准布局：FS base 指向线程控制块（TCB），TCB 首 8 字节为
 *     self 指针，故 pthread_self() 即读取 %fs:0。
 *   - 同步原语（mutex/cond）建于内核 futex（SYS_FUTEX）之上：加锁失败时在该字上
 *     futex_wait，释放时 futex_wake；join 在独立 futex 字上等待目标线程退出。
 *
 * 说明：本接口不是对 Linux glibc 的二进制兼容层——调用号与部分语义为 SukiOS 自有
 * 定义，程序在本系统需重新编译链接。
 */
#ifndef _SUKI_PTHREAD_H
#define _SUKI_PTHREAD_H

#include <stdint.h>
#include <stddef.h>

typedef struct pthread_tcb *pthread_t;

/* 属性（当前仅支持 detached 标记，默认 joinable） */
typedef struct {
    int detached;
} pthread_attr_t;

typedef volatile int  pthread_mutex_t;
typedef volatile int  pthread_cond_t;
typedef int           pthread_once_t;

#define PTHREAD_MUTEX_INITIALIZER  ((pthread_mutex_t)0)
#define PTHREAD_COND_INITIALIZER   ((pthread_cond_t)0)
#define PTHREAD_ONCE_INIT          0

/* ---- 线程生命周期 ---- */
int  pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                    void *(*start_routine)(void *), void *arg);
int  pthread_join(pthread_t thread, void **retval);
void pthread_exit(void *retval);
pthread_t pthread_self(void);
int  pthread_detach(pthread_t thread);

/* ---- 属性 ---- */
int  pthread_attr_init(pthread_attr_t *attr);
int  pthread_attr_destroy(pthread_attr_t *attr);
int  pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate);
#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

/* ---- 互斥锁 ---- */
int  pthread_mutex_init(pthread_mutex_t *mutex, const void *attr);
int  pthread_mutex_destroy(pthread_mutex_t *mutex);
int  pthread_mutex_lock(pthread_mutex_t *mutex);
int  pthread_mutex_trylock(pthread_mutex_t *mutex);
int  pthread_mutex_unlock(pthread_mutex_t *mutex);

/* ---- 条件变量 ---- */
int  pthread_cond_init(pthread_cond_t *cond, const void *attr);
int  pthread_cond_destroy(pthread_cond_t *cond);
int  pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int  pthread_cond_signal(pthread_cond_t *cond);
int  pthread_cond_broadcast(pthread_cond_t *cond);

/* ---- 一次性初始化 ---- */
int  pthread_once(pthread_once_t *once_control, void (*init_routine)(void));

#endif /* _SUKI_PTHREAD_H */
