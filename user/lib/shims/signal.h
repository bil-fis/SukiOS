/*
 * user/lib/shims/signal.h
 * -----------------------------------------------------------------------------
 * SukiOS 用户态标准 <signal.h>（供第三方库如 libcurl 包含）。
 *
 * 实现归属：user/lib/signal.c（signal/raise/sigaction/sigprocmask/sigreturn）。
 * 类型/编号与 <sukios/posix.h>（SUKI_SIG*）及 user/lib/libc.h 保持一致；
 * 与 libc.h 共用守卫宏，二者混用时不会重复定义。
 */
#ifndef _SUKI_SHIM_SIGNAL_H
#define _SUKI_SHIM_SIGNAL_H

#include <stdint.h>
#include <sukios/posix.h>   /* SUKI_SIG* 编号与类型 */

typedef int sig_atomic_t;

#ifndef _SUKI_SIGHANDLER_T_DEFINED
#define _SUKI_SIGHANDLER_T_DEFINED
typedef void (*sighandler_t)(int);
#endif
typedef struct suki_siginfo  siginfo_t;
typedef struct suki_ucontext ucontext_t;
typedef suki_sigset_t        sigset_t;

/* POSIX struct sigaction（与内核 struct suki_sigaction 二进制布局一致）。
 * 与 user/lib/libc.h 共用守卫，避免重复定义。 */
#ifndef _SUKI_STRUCT_SIGACTION_DEFINED
#define _SUKI_STRUCT_SIGACTION_DEFINED
struct sigaction {
    union {
        sighandler_t           sa_handler;
        void (*sa_sigaction)(int, siginfo_t *, void *);
    } _u;
    uint64_t        sa_flags;
    void          (*sa_restorer)(void);
    sigset_t        sa_mask;
};
#endif

/* 与 glibc 一致：以宏暴露 union 内的处理器字段（POSIX 标准写法 act.sa_handler）。 */
#ifndef sa_handler
#define sa_handler   _u.sa_handler
#endif
#ifndef sa_sigaction
#define sa_sigaction _u.sa_sigaction
#endif

/* 宏名/取值与 libc.h 完全相同（即使二者同时定义也不会告警） */
#ifndef SIG_DFL
#define SIG_DFL       SUKI_SIG_DFL
#endif
#ifndef SIG_IGN
#define SIG_IGN       SUKI_SIG_IGN
#endif
#ifndef SIG_ERR
#define SIG_ERR       SUKI_SIG_ERR
#endif
#ifndef SIGHUP
#define SIGHUP        SUKI_SIGHUP
#endif
#ifndef SIGINT
#define SIGINT        SUKI_SIGINT
#endif
#ifndef SIGQUIT
#define SIGQUIT       SUKI_SIGQUIT
#endif
#ifndef SIGILL
#define SIGILL        SUKI_SIGILL
#endif
#ifndef SIGTRAP
#define SIGTRAP       SUKI_SIGTRAP
#endif
#ifndef SIGABRT
#define SIGABRT       SUKI_SIGABRT
#endif
#ifndef SIGBUS
#define SIGBUS        SUKI_SIGBUS
#endif
#ifndef SIGFPE
#define SIGFPE        SUKI_SIGFPE
#endif
#ifndef SIGKILL
#define SIGKILL       SUKI_SIGKILL
#endif
#ifndef SIGUSR1
#define SIGUSR1       SUKI_SIGUSR1
#endif
#ifndef SIGSEGV
#define SIGSEGV       SUKI_SIGSEGV
#endif
#ifndef SIGUSR2
#define SIGUSR2       SUKI_SIGUSR2
#endif
#ifndef SIGPIPE
#define SIGPIPE       SUKI_SIGPIPE
#endif
#ifndef SIGALRM
#define SIGALRM       SUKI_SIGALRM
#endif
#ifndef SIGTERM
#define SIGTERM       SUKI_SIGTERM
#endif
#ifndef SIGCHLD
#define SIGCHLD       SUKI_SIGCHLD
#endif
#ifndef SIGCONT
#define SIGCONT       SUKI_SIGCONT
#endif
#ifndef SIGSTOP
#define SIGSTOP       SUKI_SIGSTOP
#endif
#ifndef SIGTSTP
#define SIGTSTP       SUKI_SIGTSTP
#endif
#ifndef SIGURG
#define SIGURG        SUKI_SIGURG
#endif

/* libc 信号 API（user/lib/signal.c） */
sighandler_t signal(int sig, sighandler_t handler);
int          raise(int sig);
int          kill(int pid, int sig);
int          sigaction(int sig, const struct sigaction *act, struct sigaction *oldact);
int          sigprocmask(int how, const sigset_t *set, sigset_t *oldset);

#endif /* _SUKI_SHIM_SIGNAL_H */
