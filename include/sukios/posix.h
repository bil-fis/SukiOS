/*
 * include/sukios/posix.h
 * -----------------------------------------------------------------------------
 * SukiOS POSIX 应用层二进制接口（ABI）—— 内核与用户态【唯一】共享契约。
 *
 * 设立目的：
 *   为「完整 POSIX 兼容层 + newlib 用户态 C 库」提供一套稳定的系统调用
 *   编号、参数结构体与常量定义。内核侧（kernel/syscall/sys_posix.c）与用户
 *   侧（user/lib/posix.c，后续 newlib 的 libgloss 层）都只依赖本文件，绝不
 *   各自硬编码，杜绝两侧漂移。
 *
 * 设计要点：
 *   1) 调用号【故意不】与 Linux x86_64 对齐。用户明确要求「程序兼容，调用
 *      号自定」——ELF 程序在本系统运行需重新编译（静态链接 newlib），二进制
 *      无需与 Linux 号表一致，故按功能分区重新编号，语义与 POSIX 完全一致。
 *   2) 返回值约定（Linux 惯例，newlib 直接可用）：
 *        成功：返回 >= 0 的结果值；
 *        失败：返回 -errno（errno 为正的小整数，范围 -1 .. -4095）。
 *      用户态封装据此设置 errno 并返回 -1。
 *   3) 所有结构体显式定宽、显式布局，不依赖编译器默认对齐差异；内核与用户
 *      态编译器选项一致（LP64），布局天然一致。
 *   4) 本文件 freestanding 安全：只用 <stdint.h>/<stddef.h>（-ffreestanding
 *      下由编译器提供），不依赖任何 libc。
 *
 * 调用关系：
 *   内核：kernel/syscall/syscall.c / sys_posix.c  ->  本文件的号与结构体
 *   用户：user/lib/posix.c / newlib libgloss      ->  本文件的号与结构体
 */
#ifndef _SUKI_POSIX_H
#define _SUKI_POSIX_H

#include <stdint.h>
#include <stddef.h>

/* ========================================================================== */
/*  一、基本类型（LP64，与 System V AMD64 ABI 一致）                           */
/* ========================================================================== */

typedef int64_t  suki_off_t;      /* 文件偏移（有符号） */
typedef int64_t  suki_ssize_t;    /* 有符号字节计数 */
typedef uint64_t suki_size_t;
typedef int32_t  suki_pid_t;
typedef uint32_t suki_mode_t;
typedef uint32_t suki_uid_t;
typedef uint32_t suki_gid_t;
typedef int32_t  suki_clockid_t;
typedef int64_t  suki_time_t;
typedef uint32_t suki_dev_t;
typedef uint64_t suki_ino_t;
typedef uint32_t suki_nlink_t;
typedef int32_t  suki_key_t;

/* ========================================================================== */
/*  二、errno 取值（POSIX.1-2017 全集，newlib 直接映射）                       */
/* ========================================================================== */

#define SUKI_EPERM            1    /* Operation not permitted */
#define SUKI_ENOENT           2    /* No such file or directory */
#define SUKI_ESRCH            3    /* No such process */
#define SUKI_EINTR            4    /* Interrupted system call */
#define SUKI_EIO              5    /* I/O error */
#define SUKI_ENXIO            6    /* No such device or address */
#define SUKI_E2BIG            7    /* Argument list too long */
#define SUKI_ENOEXEC          8    /* Exec format error */
#define SUKI_EBADF            9    /* Bad file descriptor */
#define SUKI_ECHILD          10    /* No child processes */
#define SUKI_EAGAIN          11    /* Try again */
#define SUKI_ENOMEM          12    /* Out of memory */
#define SUKI_EACCES          13    /* Permission denied */
#define SUKI_EFAULT          14    /* Bad address */
#define SUKI_ENOTBLK         15    /* Block device required */
#define SUKI_EBUSY           16    /* Device or resource busy */
#define SUKI_EEXIST          17    /* File exists */
#define SUKI_EXDEV           18    /* Cross-device link */
#define SUKI_ENODEV          19    /* No such device */
#define SUKI_ENOTDIR         20    /* Not a directory */
#define SUKI_EISDIR          21    /* Is a directory */
#define SUKI_EINVAL          22    /* Invalid argument */
#define SUKI_ENFILE          23    /* File table overflow */
#define SUKI_EMFILE          24    /* Too many open files */
#define SUKI_ENOTTY          25    /* Not a typewriter */
#define SUKI_ETXTBSY         26    /* Text file busy */
#define SUKI_EFBIG           27    /* File too large */
#define SUKI_ENOSPC          28    /* No space left on device */
#define SUKI_ESPIPE          29    /* Illegal seek */
#define SUKI_EROFS           30    /* Read-only file system */
#define SUKI_EMLINK          31    /* Too many links */
#define SUKI_EPIPE           32    /* Broken pipe */
#define SUKI_EDOM            33    /* Math argument out of domain */
#define SUKI_ERANGE          34    /* Math result not representable */
#define SUKI_EDEADLK         35    /* Resource deadlock would occur */
#define SUKI_ENAMETOOLONG    36    /* File name too long */
#define SUKI_ENOLCK          37    /* No record locks available */
#define SUKI_ENOSYS          38    /* Function not implemented */
#define SUKI_ENOTEMPTY       39    /* Directory not empty */
#define SUKI_ELOOP           40    /* Too many symbolic links */
#define SUKI_ENOMSG          42    /* No message of desired type */
#define SUKI_EIDRM           43    /* Identifier removed */
#define SUKI_EBADR           53    /* Invalid request descriptor */
#define SUKI_EDEADLOCK       SUKI_EDEADLK
#define SUKI_ENOSTR          60    /* Device not a stream */
#define SUKI_ENODATA         61    /* No data available */
#define SUKI_ETIME           62    /* Timer expired */
#define SUKI_ENOSR           63    /* Out of streams resources */
#define SUKI_EPROTO          71    /* Protocol error */
#define SUKI_EMULTIHOP       72    /* Multihop attempted */
#define SUKI_EBADMSG         74    /* Not a data message */
#define SUKI_EOVERFLOW       75    /* Value too large for defined data type */
#define SUKI_EILSEQ          84    /* Illegal byte sequence */
#define SUKI_ENOTSOCK        88    /* Socket operation on non-socket */
#define SUKI_EDESTADDRREQ    89    /* Destination address required */
#define SUKI_EMSGSIZE        90    /* Message too long */
#define SUKI_EPROTOTYPE      91    /* Protocol wrong type for socket */
#define SUKI_ENOPROTOOPT     92    /* Protocol not available */
#define SUKI_EPROTONOSUPPORT 93    /* Protocol not supported */
#define SUKI_ESOCKTNOSUPPORT 94    /* Socket type not supported */
#define SUKI_EOPNOTSUPP      95    /* Operation not supported on transport */
#define SUKI_EPFNOSUPPORT    96    /* Protocol family not supported */
#define SUKI_EAFNOSUPPORT    97    /* Address family not supported */
#define SUKI_EADDRINUSE      98    /* Address already in use */
#define SUKI_EADDRNOTAVAIL   99    /* Cannot assign requested address */
#define SUKI_ENETDOWN        100   /* Network is down */
#define SUKI_ENETUNREACH     101   /* Network is unreachable */
#define SUKI_ENETRESET       102   /* Network dropped connection on reset */
#define SUKI_ECONNABORTED    103   /* Software caused connection abort */
#define SUKI_ECONNRESET      104   /* Connection reset by peer */
#define SUKI_ENOBUFS         105   /* No buffer space available */
#define SUKI_EISCONN         106   /* Transport endpoint is already connected */
#define SUKI_ENOTCONN        107   /* Transport endpoint is not connected */
#define SUKI_ESHUTDOWN       108   /* Cannot send after shutdown */
#define SUKI_ETIMEDOUT       110   /* Connection timed out */
#define SUKI_ECONNREFUSED    111   /* Connection refused */
#define SUKI_EHOSTDOWN       112   /* Host is down */
#define SUKI_EHOSTUNREACH    113   /* No route to host */
#define SUKI_EALREADY        114   /* Operation already in progress */
#define SUKI_EINPROGRESS     115   /* Operation now in progress */
#define SUKI_ECANCELED       125   /* Operation canceled */

/* 失败返回值打包：-errno */
#define SUKI_ERR(n)  (-(int64_t)(n))

/* ========================================================================== */
/*  三、open/fcntl 标志（O_*）、seek、mode、mmap、文件类型                     */
/* ========================================================================== */

#define SUKI_O_RDONLY   0x0000
#define SUKI_O_WRONLY   0x0001
#define SUKI_O_RDWR     0x0002
#define SUKI_O_ACCMODE  0x0003
#define SUKI_O_CREAT    0x0100
#define SUKI_O_EXCL     0x0200
#define SUKI_O_TRUNC    0x0400
#define SUKI_O_APPEND   0x0800
#define SUKI_O_NONBLOCK 0x1000
#define SUKI_O_DIRECTORY 0x2000
#define SUKI_O_CLOEXEC  0x4000

#define SUKI_SEEK_SET   0
#define SUKI_SEEK_CUR   1
#define SUKI_SEEK_END   2

/* fcntl 命令 */
#define SUKI_F_DUPFD       0
#define SUKI_F_GETFD       1
#define SUKI_F_SETFD       2
#define SUKI_F_GETFL       3
#define SUKI_F_SETFL       4
#define SUKI_F_GETLK       5
#define SUKI_F_SETLK       6
#define SUKI_F_SETLKW      7

#define SUKI_FD_CLOEXEC    1

/* st_mode 文件类型位 */
#define SUKI_S_IFMT    0170000
#define SUKI_S_IFDIR   0040000
#define SUKI_S_IFCHR   0020000
#define SUKI_S_IFBLK   0060000
#define SUKI_S_IFREG   0100000
#define SUKI_S_IFIFO   0010000
#define SUKI_S_IFLNK   0120000
#define SUKI_S_IFSOCK  0140000

/* 权限位 */
#define SUKI_S_ISUID   0004000
#define SUKI_S_ISGID   0002000
#define SUKI_S_ISVTX   0001000
#define SUKI_S_IRWXU   0000700
#define SUKI_S_IRUSR   0000400
#define SUKI_S_IWUSR   0000200
#define SUKI_S_IXUSR   0000100
#define SUKI_S_IRWXG   0000070
#define SUKI_S_IRGRP   0000040
#define SUKI_S_IWGRP   0000020
#define SUKI_S_IXGRP   0000010
#define SUKI_S_IRWXO   0000007
#define SUKI_S_IROTH   0000004
#define SUKI_S_IWOTH   0000002
#define SUKI_S_IXOTH   0000001

/* access() mode */
#define SUKI_F_OK   0
#define SUKI_X_OK   1
#define SUKI_W_OK   2
#define SUKI_R_OK   4

/* mmap prot/flags */
#define SUKI_PROT_NONE   0x0
#define SUKI_PROT_READ   0x1
#define SUKI_PROT_WRITE  0x2
#define SUKI_PROT_EXEC   0x4

#define SUKI_MAP_SHARED    0x01
#define SUKI_MAP_PRIVATE   0x02
#define SUKI_MAP_FIXED     0x10
#define SUKI_MAP_ANONYMOUS 0x20
#define SUKI_MAP_ANON      SUKI_MAP_ANONYMOUS
#define SUKI_MAP_FAILED    ((void *)-1)

/* dirent d_type */
#define SUKI_DT_UNKNOWN  0
#define SUKI_DT_FIFO     1
#define SUKI_DT_CHR      2
#define SUKI_DT_DIR      4
#define SUKI_DT_BLK      6
#define SUKI_DT_REG      8
#define SUKI_DT_LNK      10
#define SUKI_DT_SOCK     12

/* clock id */
#define SUKI_CLOCK_REALTIME           0
#define SUKI_CLOCK_MONOTONIC          1
#define SUKI_CLOCK_PROCESS_CPUTIME_ID 2
#define SUKI_CLOCK_THREAD_CPUTIME_ID  3
#define SUKI_CLOCK_MONOTONIC_RAW      4
#define SUKI_CLOCK_REALTIME_COARSE    5
#define SUKI_CLOCK_MONOTONIC_COARSE   6
#define SUKI_CLOCK_BOOTTIME           7

/* waitpid options */
#define SUKI_WNOHANG   1
#define SUKI_WUNTRACED 2
#define SUKI_WCONTINUED 8

/* 信号编号（POSIX 兼容常量；与 Linux 取值一致，便于移植） */
#define SUKI_SIGHUP    1
#define SUKI_SIGINT    2
#define SUKI_SIGQUIT   3
#define SUKI_SIGILL    4
#define SUKI_SIGTRAP   5
#define SUKI_SIGABRT   6
#define SUKI_SIGBUS    7
#define SUKI_SIGFPE    8
#define SUKI_SIGKILL   9
#define SUKI_SIGUSR1   10
#define SUKI_SIGSEGV   11
#define SUKI_SIGUSR2   12
#define SUKI_SIGPIPE   13
#define SUKI_SIGALRM   14
#define SUKI_SIGTERM   15
#define SUKI_SIGSTKFLT 16
#define SUKI_SIGCHLD   17
#define SUKI_SIGCONT   18
#define SUKI_SIGSTOP   19
#define SUKI_SIGTSTP   20
#define SUKI_SIGTTIN   21
#define SUKI_SIGTTOU   22
#define SUKI_SIGURG    23
#define SUKI_SIGXCPU   24
#define SUKI_SIGXFSZ   25
#define SUKI_SIGVTALRM 26
#define SUKI_SIGPROF   27
#define SUKI_SIGWINCH  28
#define SUKI_SIGPOLL   29
#define SUKI_SIGPWR    30
#define SUKI_SIGSYS    31
#define SUKI_NSIG      64   /* 信号总数（含 1..63 可用；0 为预留） */

/* 信号处置（sa_handler 取值） */
#define SUKI_SIG_DFL   ((void (*)(int))0)   /* 默认动作 */
#define SUKI_SIG_IGN   ((void (*)(int))1)   /* 忽略 */
#define SUKI_SIG_ERR   ((void (*)(int))-1)  /* 错误返回 */

/* sigaction 标志 */
#define SUKI_SA_NOCLDSTOP 0x00000001
#define SUKI_SA_RESTART    0x00000002
#define SUKI_SA_NOCLDWAIT  0x00000008
#define SUKI_SA_RESETHAND  0x00000004
#define SUKI_SA_SIGINFO    0x00000040
#define SUKI_SA_NODEFER    0x00000020
#define SUKI_SA_RESTORER   0x04000000

/* sigprocmask how */
#define SUKI_SIG_BLOCK   0
#define SUKI_SIG_UNBLOCK 1
#define SUKI_SIG_SETMASK 2

/* siginfo code（常用） */
#define SUKI_SI_USER     0
#define SUKI_SI_KERNEL   128
#define SUKI_SI_QUEUE    1
#define SUKI_SI_TIMER    2
#define SUKI_SI_TKILL    4

/* 信号集：本实现以 64 位位掩码表示（bit (sig-1) 置位） */
typedef uint64_t suki_sigset_t;

/* struct sigaction（简化但可移植；sa_handler 与 sa_sigaction 共用首字段） */
struct suki_sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, struct suki_siginfo *, struct suki_ucontext *);
    } _u;
    uint64_t        sa_flags;
    void          (*sa_restorer)(void);
    suki_sigset_t   sa_mask;
};

/* struct siginfo（简化：覆盖 signo/code/pid/uid/status/value） */
struct suki_siginfo {
    int    si_signo;
    int    si_code;
    int    si_errno;
    int    _pad0;
    int64_t si_pid;
    int64_t si_uid;
    int64_t si_status;
    int64_t si_value;
};

/* 用户态上下文（信号帧与 sigreturn 还原用）。
 * gpr 顺序与内核返回帧 GPR 槽一致：r9,r8,r10,rdx,rsi,rdi,r15,r14,r13,r12,rbp,rbx */
struct suki_ucontext {
    uint64_t gpr[12];
    uint64_t rax;
    uint64_t rip;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t cr2;
    suki_sigset_t sigmask;
    uint8_t  fpu[512] __attribute__((aligned(16)));   /* fxsave/fxrstor 要求 16 字节对齐 */
};

/* socket（号位预留；协议栈未就绪时按 POSIX 语义返回 -ENOSYS） */
#define SUKI_AF_UNIX   1
#define SUKI_AF_INET   2
#define SUKI_AF_INET6  10
#define SUKI_SOCK_STREAM 1
#define SUKI_SOCK_DGRAM  2
#define SUKI_SOCK_RAW    3

/* sysconf 名字（_SC_*） */
#define SUKI_SC_PAGESIZE        30
#define SUKI_SC_PAGE_SIZE       SUKI_SC_PAGESIZE
#define SUKI_SC_NPROCESSORS_ONLN 84
#define SUKI_SC_PHYS_PAGES      85
#define SUKI_SC_AVPHYS_PAGES    86
#define SUKI_SC_OPEN_MAX        4
#define SUKI_SC_CLK_TCK         2
#define SUKI_SC_ARG_MAX         0
#define SUKI_SC_HOST_NAME_MAX   180

/* ========================================================================== */
/*  四、内核/用户共享数据结构                                                  */
/* ========================================================================== */

/* struct stat：显式定宽，避免两侧对齐差异 */
typedef struct suki_stat {
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
} suki_stat_t;

/* struct dirent / getdents：name 定长 256，d_reclen 为对齐后记录长度 */
typedef struct suki_dirent {
    uint64_t d_ino;
    int64_t  d_off;        /* 下一条记录在目录流中的偏移 */
    uint16_t d_reclen;
    uint8_t  d_type;
    uint8_t  _pad[5];
    char     d_name[256];
} suki_dirent_t;

typedef struct suki_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
} suki_timespec_t;

typedef struct suki_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
} suki_timeval_t;

typedef struct suki_timezone {
    int32_t tz_minuteswest;
    int32_t tz_dsttime;
} suki_timezone_t;

typedef struct suki_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
} suki_utsname_t;

typedef struct suki_tms {
    int64_t tms_utime;
    int64_t tms_stime;
    int64_t tms_cutime;
    int64_t tms_cstime;
} suki_tms_t;

typedef struct suki_iovec {
    void   *iov_base;
    uint64_t iov_len;
} suki_iovec_t;

typedef struct suki_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
} suki_pollfd_t;

#define SUKI_POLLIN    0x001
#define SUKI_POLLPRI   0x002
#define SUKI_POLLOUT   0x004
#define SUKI_POLLERR   0x008
#define SUKI_POLLHUP   0x010
#define SUKI_POLLNVAL  0x020

typedef struct suki_rusage {
    int64_t ru_utime_sec;
    int64_t ru_utime_usec;
    int64_t ru_stime_sec;
    int64_t ru_stime_usec;
    int64_t ru_maxrss;
    int64_t ru_ixrss;
    int64_t ru_idrss;
    int64_t ru_isrss;
    int64_t ru_minflt;
    int64_t ru_majflt;
    int64_t ru_nswap;
    int64_t ru_inblock;
    int64_t ru_oublock;
    int64_t ru_msgsnd;
    int64_t ru_msgrcv;
    int64_t ru_nsignals;
    int64_t ru_nvcsw;
    int64_t ru_nivcsw;
} suki_rusage_t;

#define SUKI_RUSAGE_SELF     0
#define SUKI_RUSAGE_CHILDREN -1

typedef struct suki_rlimit {
    uint64_t rlim_cur;
    uint64_t rlim_max;
} suki_rlimit_t;

#define SUKI_RLIMIT_NOFILE   7
#define SUKI_RLIMIT_STACK    3
#define SUKI_RLIMIT_AS       9
#define SUKI_RLIMIT_DATA     2
#define SUKI_RLIMIT_CORE     4
#define SUKI_RLIMIT_CPU      0

typedef struct suki_sysinfo {
    int64_t uptime;
    uint64_t loads[3];
    uint64_t totalram;
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t bufferram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs;
    uint16_t pad;
    uint32_t pad2;
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t mem_unit;
} suki_sysinfo_t;

typedef struct suki_statfs {
    uint64_t f_type;
    uint64_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    uint64_t f_fsid[2];
    uint64_t f_namelen;
    uint64_t f_frsize;
    uint64_t f_flags;
} suki_statfs_t;

/* fd_set（select 用；固定 1024 位 = 128 字节） */
#define SUKI_FD_SETSIZE 1024
typedef struct suki_fd_set {
    uint64_t bits[SUKI_FD_SETSIZE / 64];
} suki_fd_set_t;

/* ========================================================================== */
/*  五、系统调用号（分区编号，语义对齐 POSIX）                                   */
/* ========================================================================== */

/* ---- A. SukiOS / Mach 原生（0..19）---- */
#define SYS_MACH_MSG        0
#define SYS_TASK_SPAWN      1
#define SYS_TASK_EXIT       2
#define SYS_YIELD           3
#define SYS_DEBUG_WRITE     4
#define SYS_INPUT_READ      5
#define SYS_REBOOT          6
#define SYS_PORT_CLAIM      7
/* 注意：17/18 已被核心 ABI 占用（SYS_FORK/SYS_GETPID），绝不可复用，否则
 * syscall_dispatch 的 switch 会优先命中 SYS_PORT_ALLOC/FREE 而拦截 fork/getpid，
 * 造成全局进程管理失效（getpid 恒返回 0、fork 返回端口号）。故动态端口分配/
 * 释放使用 88..129 区段末端的空闲号位 97/98。 */
#define SYS_PORT_ALLOC      97   /* 分配一个动态接收端口，返回端口号（0=失败） */
#define SYS_PORT_FREE       98   /* 释放动态端口（a1=端口号） */
#define SYS_EXECVE          8
#define SYS_WAIT            9
#define SYS_AUDIO_OPEN      10
#define SYS_AUDIO_WRITE     11
#define SYS_AUDIO_QUEUED    12
#define SYS_AUDIO_STOP      13
#define SYS_MMAP_LEGACY     14   /* 旧两参匿名映射，保留兼容 */
#define SYS_MUNMAP_LEGACY   15   /* 旧两参解除映射，保留兼容 */
#define SYS_SERIAL_READ     16
#define SYS_FORK            17
#define SYS_GETPID          18
#define SYS_GETPPID         19

/* ---- B. 进程 / 调度 / 资源（20..39）---- */
#define SYS_WAITPID         20
#define SYS_KILL            21
#define SYS_GETUID          22
#define SYS_GETGID          23
#define SYS_GETEUID         24
#define SYS_GETEGID         25
#define SYS_SETUID          26
#define SYS_SETGID          27
#define SYS_BRK             28
#define SYS_SBRK            29
#define SYS_TIMES           30
#define SYS_UMASK           31
#define SYS_GETTID          32
#define SYS_EXIT_GROUP      33
#define SYS_SET_TID_ADDRESS 34
#define SYS_ARCH_PRCTL      35
#define SYS_FUTEX           36
#define SYS_GETRLIMIT       37
#define SYS_SETRLIMIT       38
#define SYS_GETRUSAGE       39

/* ---- C. 文件与目录 I/O（40..89）---- */
#define SYS_OPEN            40
#define SYS_CLOSE           41
#define SYS_READ            42
#define SYS_WRITE           43
#define SYS_LSEEK           44
#define SYS_STAT            45
#define SYS_FSTAT           46
#define SYS_LSTAT           47
#define SYS_UNLINK          48
#define SYS_MKDIR           49
#define SYS_RMDIR           50
#define SYS_OPENDIR         51
#define SYS_READDIR         52
#define SYS_CLOSEDIR        53
#define SYS_DUP             54
#define SYS_DUP2            55
#define SYS_FCNTL           56
#define SYS_ACCESS          57
#define SYS_RENAME          58
#define SYS_TRUNCATE        59
#define SYS_FTRUNCATE       60
#define SYS_CHDIR           61
#define SYS_GETCWD          62
#define SYS_PIPE            63
#define SYS_IOCTL           64
#define SYS_LINK            65
#define SYS_SYMLINK         66
#define SYS_READLINK        67
#define SYS_CHMOD           68
#define SYS_FCHMOD          69
#define SYS_SYNC            70
#define SYS_FSYNC           71
#define SYS_UTIMES          72
#define SYS_GETDENTS        73
#define SYS_ISATTY          74
#define SYS_STATFS          75
#define SYS_FSTATFS         76
#define SYS_PREAD           77
#define SYS_PWRITE          78
#define SYS_READV           79
#define SYS_WRITEV          80
#define SYS_SELECT          81
#define SYS_POLL            82
#define SYS_REALPATH        83
#define SYS_MKNOD           84
#define SYS_CHOWN           85
#define SYS_FCHOWN          86
#define SYS_TELLDIR         87
#define SYS_SEEKDIR         88
#define SYS_FPATHCONF       89

/* ---- D. 内存映射（90..99）---- */
#define SYS_MMAP            90
#define SYS_MUNMAP          91
#define SYS_MPROTECT        92
#define SYS_MSYNC           93
#define SYS_MADVISE         94
#define SYS_MINCORE         95
#define SYS_MREMAP          96

/* ---- E. 时间（100..109）---- */
#define SYS_CLOCK_GETTIME   100
#define SYS_CLOCK_SETTIME   101
#define SYS_CLOCK_GETRES    102
#define SYS_GETTIMEOFDAY    103
#define SYS_NANOSLEEP       104
#define SYS_TIME            105
#define SYS_SETTIMEOFDAY    106
#define SYS_ALARM           107
#define SYS_GETITIMER       108
#define SYS_SETITIMER       109

/* ---- F. 系统信息与杂项（110..129）---- */
#define SYS_UNAME           110
#define SYS_SYSINFO         111
#define SYS_GETRANDOM       112
#define SYS_SYSCONF         113
#define SYS_PRCTL           114
#define SYS_GETHOSTNAME     115
#define SYS_SETHOSTNAME     116
#define SYS_GETPGRP         117
#define SYS_SETPGRP         118
#define SYS_GETSID          119
#define SYS_NICE            120
#define SYS_GETPRIORITY     121
#define SYS_SETPRIORITY     122
#define SYS_SCHED_GETPID    123
#define SYS_MPROTECT_KEY    124
#define SYS_OOL_UNMAP       125   /* 释放 mach_msg OOL 接收窗口（va） */
/* ---- G. SukiNative 原生对象 API（130..149）----
 * 与 POSIX（20..129）【平行】的原生接口：一切皆对象，返回 suki_handle_t 句柄，
 * 用 suki_status_t 状态码（而非 errno）。POSIX 兼容层对外号位与行为保持不变；
 * 新服务可直接使用 SukiNative。当前 Phase 0 仅完成号位预留与分发路由
 * （sys_suki_dispatch），具体对象/句柄子系统在 Phase 1 实现，未实现号返回 -ENOSYS。 */
#define SYS_SUKI_OBJ_CREATE     130
#define SYS_SUKI_OBJ_DESTROY    131
#define SYS_SUKI_OBJ_DUPLICATE  132
#define SYS_SUKI_OBJ_QUERY      133
#define SYS_SUKI_WAIT           134
#define SYS_SUKI_EVENT_CREATE   135
#define SYS_SUKI_EVENT_SET      136
#define SYS_SUKI_EVENT_RESET    137
#define SYS_SUKI_MUTEX_CREATE   138
#define SYS_SUKI_MUTEX_LOCK     139
#define SYS_SUKI_MUTEX_UNLOCK   140
#define SYS_SUKI_SEM_CREATE     141
#define SYS_SUKI_SEM_ACQUIRE    142
#define SYS_SUKI_SEM_RELEASE    143
#define SYS_SUKI_FILE_OPEN      144
#define SYS_SUKI_FILE_READ      145
#define SYS_SUKI_FILE_WRITE     146
#define SYS_SUKI_FILE_CLOSE     147
#define SYS_SUKI_PROC_CREATE    148
#define SYS_SUKI_MEM_ALLOC      149

/* ---- G'（续）SukiNative ABI 公共类型（内核/用户共享，单一真相源）---- */
typedef enum {
    SUKI_OT_NONE  = 0,
    SUKI_OT_EVENT,
    SUKI_OT_MUTEX,
    SUKI_OT_SEM,
    SUKI_OT_FILE,
    SUKI_OT_PROC,
    SUKI_OT_MEM,
} suki_obj_type_t;

/* 句柄：每任务句柄表索引 +1（0 保留为非法）。 */
typedef uint32_t suki_handle_t;

/* SYS_SUKI_WAIT 的 a3 flags */
#define SUKI_WAIT_ANY       0x1
#define SUKI_WAIT_ALL       0x2
#define SUKI_WAIT_NO_BLOCK  0x4

/* 对象查询信息（SYS_SUKI_OBJ_QUERY 输出；内核/用户布局一致） */
typedef struct suki_objinfo {
    uint32_t type;     /* suki_obj_type_t */
    uint32_t state;    /* event:1=signaled; mutex:1=locked; sem:1=有可用计数;
                        * proc:1=子进程已退出 */
    int32_t  count;    /* sem 当前计数（其余类型 0） */
    uint32_t _pad;
    uint64_t base;     /* MEM：映射的用户基地址；PROC：子进程 pid（复用） */
    uint64_t size;     /* MEM：映射区字节数；其余类型 0 */
} suki_objinfo_t;

/* ---- H. 网络（150..169）：socket 号位预留（协议栈未就绪，调用返回 -ENOSYS）---- */
#define SYS_SOCKET          150
#define SYS_BIND            151
#define SYS_CONNECT         152
#define SYS_LISTEN          153
#define SYS_ACCEPT          154
#define SYS_SENDTO          155
#define SYS_RECVFROM        156
#define SYS_SENDMSG         157
#define SYS_RECVMSG         158
#define SYS_SHUTDOWN        159
#define SYS_SETSOCKOPT      160
#define SYS_GETSOCKOPT      161
#define SYS_GETPEERNAME     162
#define SYS_GETSOCKNAME     163
#define SYS_SOCKETPAIR      164
#define SYS_SEND            165   /* 已连接 socket 发送（等价于 sendto 无对端地址） */
#define SYS_RECV            166   /* 已连接 socket 接收（等价于 recvfrom 无对端地址） */

/* 内核扩展：Ring3 显示服务经此系统调用获得帧缓冲用户态映射与显示配置。
 * 放在原生区之外的独立扩展号，避免与 POSIX 区（20+）混淆。 */
#define SYS_FRAMEBUFFER_MAP 200

/* 内核扩展：显示服务完成「桌面合成 + 进入消息循环」前调用，通知内核它已
 * 准备好接收经 DISPLAY_PORT 转发来的控制台文本。此后内核 user_puts() 不再
 * 直接写帧缓冲（避免覆盖显示服务的合成画面），改为经 IPC 把文本交给显示
 * 服务渲染到桌面内的终端窗口。仅作握手标志，无参数、无返回值语义。 */
#define SYS_DISPLAY_READY   201

/* 内核扩展：读取「早期控制台环形管道」累积的内核启动日志（类似 Unix dmesg）。
 * 显示服务接管帧缓冲后，内核诊断不再直接写屏，而是被捕获进内核环形管道，
 * 供后续用户态 shell（或任意进程）经此调用取回日志用于显示/调试。
 *   参数 a1 = 用户态缓冲指针，a2 = 缓冲字节数上限；
 *   返回实际拷贝字节数（0=暂无数据），非法指针返回 (uint64_t)-1。 */
#define SYS_CONSOLE_READ    202

/* 内核扩展：Ring3 程序（如 BMP 加载器）请求把一块像素 blit 到帧缓冲，用于显示
 * 诊断（检查画面乱码/错位）。内核拥有帧缓冲内核映射，直接写入。
 *   参数 a1 = 用户态像素缓冲（uint32_t*，xRGB32 格式：(r<<16)|(g<<8)|b）；
 *        a2 = 宽（像素），a3 = 高（像素），a4 = 目标 x，a5 = 目标 y；
 *   返回 0 成功，非法参数/指针返回 (uint64_t)-1。单次 blit 上限 4MiB 像素字节。 */
#define SYS_DISPLAY_BLIT    203

/* 八、线程 / 克隆（pthread 基础，SYS_CLONE 走 native 分发，不入 >=204 的 posix 表） */
#define SYS_CLONE           205   /* 创建线程/子进程：共享地址空间 + 自定义入口（trampoline） */

/* 九、信号（signal/sigaction/raise/kill 用户态处理器投递） */
#define SYS_SIGACTION       206   /* sigaction(sig, act, oldact) */
#define SYS_SIGRETURN       207   /* sigreturn(ucontext*)：还原被信号打断的上下文 */
#define SYS_SIGPROCMASK     208   /* sigprocmask(how, set, oldset) */
#define SYS_TKILL           209   /* tkill(tid, sig)：向指定线程发送信号 */
#define SYS_RAISE           210   /* raise(sig)：向自身发送信号 */
#define SUKI_ARCH_SET_FS    0x1002  /* arch_prctl：设置 FS base（TLS 基址，x86_64） */
#define SUKI_ARCH_GET_FS    0x1003  /* arch_prctl：读取 FS base */
/* sys_clone flags（SukiOS 自有定义；本系统程序重编译，不追求与 Linux 完全一致） */
#define SUKI_CLONE_VM             0x0001  /* 共享地址空间（线程语义；否则按 fork 复制） */
#define SUKI_CLONE_FILES          0x0002  /* 共享 fd 槽表 */
#define SUKI_CLONE_THREAD         0x0004  /* 共享 tgid（getpid 返回线程组组长 pid） */
#define SUKI_CLONE_SETTLS         0x0008  /* tls 有效，作为子线程 FS base */
#define SUKI_CLONE_CHILD_SETTID   0x0010  /* 将子线程 tid 写入用户 *ptid */
#define SUKI_CLONE_CHILD_CLEARTID 0x0020  /* 子线程退出时清零用户 *ctid 并 futex_wake */
/* 204: sys_mouse_read —— 非阻塞取一个解析后的鼠标事件；无数据返回 (uint64_t)-1。
 *    a1 = 用户态 mouse_packet_t*（内核填 dx/dy/buttons/wheel 后 copy_to_user 写回）；
 *    成功返回 0，失败（指针非法）返回 (uint64_t)-1。事件语义见 kernel/mouse.h。 */
#define SYS_MOUSE_READ      204

#define SYSCALL_MAX         204

/* ========================================================================== */
/*  六、每进程资源上限（内核 fd 表规模等）                                     */
/* ========================================================================== */

#define SUKI_FD_MAX         32     /* 每进程最大打开文件描述符数 */
#define SUKI_PATH_MAX       4096
#define SUKI_NAME_MAX       255

/* ========================================================================== */
/*  七、用户态 syscall 内联封装（用户态使用；内核侧不调用）                     */
/* ========================================================================== */
/*
 * 寄存器约定（System V AMD64 + syscall 指令）：
 *   rax = 调用号；rdi, rsi, rdx, r10, r8, r9 = 参数 1..6；返回值在 rax。
 * 注意第 4 参数走 r10（不是 rcx）——syscall 指令自身用 rcx 保存返回 RIP、
 * r11 保存 RFLAGS，故 rcx 不可作为参数寄存器。
 *
 * clobber 说明（关键，曾致真实故障）：必须覆盖 rcx/r11（syscall 破坏）以及
 * rbx/r9；否则编译器可能把循环不变量（如恒定调用号）缓存在被踩坏的寄存器
 * 中，产生海量非法调用号。
 */

#ifndef SUKI_KERNEL_BUILD

static inline int64_t __suki_syscall0(int64_t n)
{
    int64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n)
                     : "rcx", "r11", "rbx", "r9", "memory");
    return ret;
}

static inline int64_t __suki_syscall1(int64_t n, int64_t a1)
{
    int64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a1)
                     : "rcx", "r11", "rbx", "r9", "memory");
    return ret;
}

static inline int64_t __suki_syscall2(int64_t n, int64_t a1, int64_t a2)
{
    int64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2)
                     : "rcx", "r11", "rbx", "r9", "memory");
    return ret;
}

static inline int64_t __suki_syscall3(int64_t n, int64_t a1, int64_t a2,
                                      int64_t a3)
{
    int64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "rbx", "r9", "memory");
    return ret;
}

static inline int64_t __suki_syscall4(int64_t n, int64_t a1, int64_t a2,
                                      int64_t a3, int64_t a4)
{
    int64_t ret;
    register int64_t r10 __asm__("r10") = a4;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "rbx", "r9", "memory");
    return ret;
}

static inline int64_t __suki_syscall5(int64_t n, int64_t a1, int64_t a2,
                                      int64_t a3, int64_t a4, int64_t a5)
{
    int64_t ret;
    register int64_t r10 __asm__("r10") = a4;
    register int64_t r8  __asm__("r8")  = a5;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                     : "rcx", "r11", "rbx", "r9", "memory");
    return ret;
}

static inline int64_t __suki_syscall6(int64_t n, int64_t a1, int64_t a2,
                                      int64_t a3, int64_t a4, int64_t a5,
                                      int64_t a6)
{
    int64_t ret;
    register int64_t r10 __asm__("r10") = a4;
    register int64_t r8  __asm__("r8")  = a5;
    register int64_t r9  __asm__("r9")  = a6;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                       "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "rbx", "memory");
    return ret;
}

#endif /* !SUKI_KERNEL_BUILD */

#endif /* _SUKI_POSIX_H */
