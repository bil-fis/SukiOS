/*
 * user/lib/libc.h
 * -----------------------------------------------------------------------------
 * SukiOS 用户态 C 库（libc）汇总头。
 *
 * 设计：标准 C / POSIX 函数的「声明」统一放在 user/lib/shims/ 下的标准头
 * （<string.h>/<stdlib.h>/<stdio.h>/<unistd.h>/<errno.h>/<ctype.h>/<time.h>/
 * <dirent.h>），由 freestanding 交叉编译器经 -I user/lib/shims 找到；本文件
 * 只在标准头基础上补充 SukiOS 特有的【类型/结构体/常量宏/errno 映射/getopt】，
 * 避免重复声明（单一真相源原则）。
 *
 * 所有 syscall 号、errno 码、POSIX 常量均取自 <sukios/posix.h>（内核/用户共享，
 * 唯一真相源），本文件与 suki.h 均不各自硬编码。
 */
#ifndef _SUKI_USER_LIBC_H
#define _SUKI_USER_LIBC_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>

/* ---- 标准 C / POSIX 函数声明（shims 下完整标准头，与 libc 实现一致） ---- */
#include <errno.h>     /* errno 外部变量 + E* 错误码宏 */
#include <string.h>    /* mem* / str* + strerror/strsignal */
#include <stdlib.h>    /* malloc/free/exit/getenv/... + atexit */
#include <stdio.h>     /* printf/puts/... */
#include <unistd.h>    /* open/read/write/fork/... + POSIX 常量 */
#include <ctype.h>     /* isalpha/toupper/... */
#include <time.h>      /* time/gettimeofday/clock_gettime/... */
#include <dirent.h>    /* DIR/struct dirent/opendir/... */

#include "suki.h"      /* syscall 封装、mach_msg、端口号等 SukiOS 专有 API */

/* ========================================================================== */
/*  SukiOS 特有结构体（标准头未覆盖；time/dirent/pid_t 等基础类型已由对应       */
/*  标准头 <time.h>/<dirent.h>/<unistd.h> 提供，此处不再重复定义以免冲突）        */
/* ========================================================================== */
struct timezone {
    int32_t tz_minuteswest;
    int32_t tz_dsttime;
};
#ifndef _SUKI_STRUCT_STAT_DEFINED
#define _SUKI_STRUCT_STAT_DEFINED
struct stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    int64_t  st_atim_sec;
    int64_t  st_atim_nsec;
    int64_t  st_mtim_sec;
    int64_t  st_mtim_nsec;
    int64_t  st_ctim_sec;
    int64_t  st_ctim_nsec;
};
#endif
struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};
struct tms {
    int64_t tms_utime;
    int64_t tms_stime;
    int64_t tms_cutime;
    int64_t tms_cstime;
};
struct rlimit {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};
typedef struct utsname  utsname;
typedef struct tms      tms;
typedef struct rlimit   rlimit;

/* 把内核返回的 -errno 转成「设置 errno 并返回 -1」的标准 POSIX 语义 */
static inline long libc_ret(long r)
{
    if (r < 0 && r >= -4095) {
        errno = (int)(-r);
        return -1;
    }
    return r;
}

/* ---- 堆（sys_brk 后端，声明见 <stdlib.h>） ---- */

/* ---- 进程 / 文件（POSIX 名字，声明见 <unistd.h>/<stdlib.h>） ---- */

/* ---- 目录流（声明见 <dirent.h>） ---- */

/* ---- 环境变量（进程内维护，声明见 <stdlib.h>） ---- */

/* ---- 时间（声明见 <time.h>） ---- */

/* ---- 系统 ---- */
int uname(struct utsname *buf);

/* 标准 errno 错误码名已在 <errno.h> 提供，此处不再重复定义。 */

/* 常量宏（POSIX 程序直接用；值取自 sukios/posix.h 唯一真相源） */
#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

#define O_RDONLY   SUKI_O_RDONLY
#define O_WRONLY   SUKI_O_WRONLY
#define O_RDWR     SUKI_O_RDWR
#define O_CREAT    SUKI_O_CREAT
#define O_EXCL     SUKI_O_EXCL
#define O_TRUNC    SUKI_O_TRUNC
#define O_APPEND   SUKI_O_APPEND
#define O_NONBLOCK SUKI_O_NONBLOCK
#define O_DIRECTORY SUKI_O_DIRECTORY

#define SEEK_SET   SUKI_SEEK_SET
#define SEEK_CUR   SUKI_SEEK_CUR
#define SEEK_END   SUKI_SEEK_END

#define F_OK  SUKI_F_OK
#define R_OK  SUKI_R_OK
#define W_OK  SUKI_W_OK
#define X_OK  SUKI_X_OK

#define PROT_READ   SUKI_PROT_READ
#define PROT_WRITE  SUKI_PROT_WRITE
#define PROT_EXEC   SUKI_PROT_EXEC
#define PROT_NONE   SUKI_PROT_NONE

#define MAP_SHARED     SUKI_MAP_SHARED
#define MAP_PRIVATE    SUKI_MAP_PRIVATE
#define MAP_FIXED      SUKI_MAP_FIXED
#define MAP_ANONYMOUS  SUKI_MAP_ANONYMOUS
#define MAP_ANON       SUKI_MAP_ANON
#define MAP_FAILED     SUKI_MAP_FAILED

#define WNOHANG  SUKI_WNOHANG

#define S_IFMT   SUKI_S_IFMT
#define S_IFDIR  SUKI_S_IFDIR
#define S_IFREG  SUKI_S_IFREG
#define S_IFCHR  SUKI_S_IFCHR
#define S_IFBLK  SUKI_S_IFBLK
#define S_IFIFO  SUKI_S_IFIFO
#define S_IFLNK  SUKI_S_IFLNK
#define S_IFSOCK SUKI_S_IFSOCK

#define S_ISDIR(m)  (((m) & SUKI_S_IFMT) == SUKI_S_IFDIR)
#define S_ISREG(m)  (((m) & SUKI_S_IFMT) == SUKI_S_IFREG)

/* ---- 信号（signal/sigaction，用户态 handler 投递） ---- */
typedef void (*sighandler_t)(int);
typedef struct suki_siginfo  siginfo_t;
typedef struct suki_ucontext ucontext_t;
typedef suki_sigset_t        sigset_t;

/* POSIX struct sigaction（与内核 struct suki_sigaction 二进制布局一致）。
 * 必须以 struct 标签形式定义：struct 标签与下方函数名 sigaction 处于不同命名
 * 空间，不冲突；若用 typedef 名 sigaction 则会与普通标识符（函数名）冲突，
 * 触发「redefinition of sigaction」编译错误。与 <signal.h> 共用守卫宏。 */
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

/* 与 glibc 一致：以宏暴露 union 内的处理器字段，使第三方代码可直接写
 * `act.sa_handler = ...`（POSIX 标准写法）。此宏仅用户态头定义，内核不包含它。 */
#ifndef sa_handler
#define sa_handler   _u.sa_handler
#endif
#ifndef sa_sigaction
#define sa_sigaction _u.sa_sigaction
#endif

#define SIG_DFL       SUKI_SIG_DFL
#define SIG_IGN       SUKI_SIG_IGN
#define SIG_ERR       SUKI_SIG_ERR
#define SA_SIGINFO    SUKI_SA_SIGINFO
#define SA_RESTART    SUKI_SA_RESTART
#define SA_RESETHAND  SUKI_SA_RESETHAND
#define SA_NODEFER    SUKI_SA_NODEFER
#define SA_RESTORER   SUKI_SA_RESTORER
#define SIG_BLOCK     SUKI_SIG_BLOCK
#define SIG_UNBLOCK   SUKI_SIG_UNBLOCK
#define SIG_SETMASK   SUKI_SIG_SETMASK

#define SIGHUP    SUKI_SIGHUP
#define SIGINT    SUKI_SIGINT
#define SIGQUIT   SUKI_SIGQUIT
#define SIGILL    SUKI_SIGILL
#define SIGTRAP   SUKI_SIGTRAP
#define SIGABRT   SUKI_SIGABRT
#define SIGBUS    SUKI_SIGBUS
#define SIGFPE    SUKI_SIGFPE
#define SIGKILL   SUKI_SIGKILL
#define SIGUSR1   SUKI_SIGUSR1
#define SIGSEGV   SUKI_SIGSEGV
#define SIGUSR2   SUKI_SIGUSR2
#define SIGPIPE   SUKI_SIGPIPE
#define SIGALRM   SUKI_SIGALRM
#define SIGTERM   SUKI_SIGTERM
#define SIGCHLD   SUKI_SIGCHLD
#define SIGCONT   SUKI_SIGCONT
#define SIGSTOP   SUKI_SIGSTOP
#define SIGTSTP   SUKI_SIGTSTP
#define SIGURG    SUKI_SIGURG
#define NSIG      SUKI_NSIG

/* libc 信号 API（实现见 lib/signal.c） */
int    sigaction(int sig, const struct sigaction *act, struct sigaction *oldact);
int    sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int    kill(int pid, int sig);
int    raise(int sig);
sighandler_t signal(int sig, sighandler_t handler);
void   sigreturn(const void *ucontext);
#define S_ISCHR(m)  (((m) & SUKI_S_IFMT) == SUKI_S_IFCHR)
#define S_ISBLK(m)  (((m) & SUKI_S_IFMT) == SUKI_S_IFBLK)
#define S_ISFIFO(m) (((m) & SUKI_S_IFMT) == SUKI_S_IFIFO)
#define S_ISLNK(m)  (((m) & SUKI_S_IFMT) == SUKI_S_IFLNK)

#define WIFEXITED(s)   (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFSIGNALED(s) (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)
#define WTERMSIG(s)    ((s) & 0x7f)

#define PATH_MAX  SUKI_PATH_MAX
#define NAME_MAX  SUKI_NAME_MAX

/* ---- getopt / getopt_long（命令行参数解析，bash 风格） ---- */
struct option {
    const char *name;     /* 长选项名（不含前导 --） */
    int         has_arg;  /* no_argument / required_argument / optional_argument */
    int        *flag;     /* 非 NULL 时把 val 写入 *flag 并返回 0，否则返回 val */
    int         val;      /* flag==NULL 时作为返回值 */
};
#define no_argument       0
#define required_argument 1
#define optional_argument 2

extern char *optarg;
extern int   optind;
extern int   opterr;
extern int   optopt;

int getopt(int argc, char *const argv[], const char *optstring);
int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex);

/* ---- 线程 / TLS / futex 底层 syscall 封装（pthread.c 复用） ---- */
/* clone：创建共享地址空间的线程或新进程。trampoline 为子线程用户态入口，
 * child_stack 为子线程用户栈顶，tls 为子线程 FS base（TCB 地址）。
 * 返回父进程的子系统 tid；子线程不经过本调用返回，直接执行 trampoline。 */
long clone(unsigned long flags, void *child_stack, void *trampoline,
           void *tls, void *ptid, void *ctid);

/* futex：op=0 等待（*uaddr==val 才睡），op=1 唤醒最多 val 个等待者 */
int futex(uint32_t *uaddr, int op, uint32_t val, const void *timeout,
          uint32_t *uaddr2, uint32_t val3);

/* arch_prctl：x86_64 专用，code=SUKI_ARCH_SET_FS/GET_FS，addr 为 FS base 或接收缓冲 */
long arch_prctl(int code, void *addr);

/* gettid / set_tid_address：线程 id 与退出清零地址登记 */
int  gettid(void);
int  set_tid_address(int *tidptr);

/* POSIX mmap / munmap：六参 mmap 薄封装（底层 SYS_MMAP=90 / SYS_MUNMAP=91） */
void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off);
int  munmap(void *addr, size_t len);

#endif /* _SUKI_USER_LIBC_H */
