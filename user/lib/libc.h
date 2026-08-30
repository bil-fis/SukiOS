/*
 * user/lib/libc.h
 * -----------------------------------------------------------------------------
 * SukiOS 用户态 C 库（libc）内部汇总头。
 *
 * 本库是「对接 SukiOS 自有 syscall 号表（include/sukios/posix.h）」的完整 C
 * 运行库，供用户态程序（含后续从 Linux 迁移、经重新编译的程序）静态链接使用。
 * 设计参照 newlib 的 libc/syscalls 模式：库函数最终经 suki.h 的 syscall 封装
 * 落到内核；标准 C 与 POSIX 名字（printf/open/fork/malloc...）全部在此提供，
 * 使程序无需区分「裸 syscall」与「libc 包装」。
 *
 * freestanding 安全：仅依赖编译器内置 <stdarg.h>/<stddef.h>/<stdint.h>，不依赖
 * 任何外部 libc。
 */
#ifndef _SUKI_USER_LIBC_H
#define _SUKI_USER_LIBC_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include "suki.h"

/* ---- errno（单线程用户态：每个任务独立镜像，进程内全局即可） ---- */
extern int errno;
int *__errno_location(void);

/* ========================================================================== */
/*  标准 C / POSIX 类型与结构体（必须在函数声明之前定义）                       */
/* ========================================================================== */
typedef suki_pid_t    pid_t;
typedef suki_off_t    off_t;
typedef suki_ssize_t  ssize_t;
typedef suki_mode_t   mode_t;
typedef suki_uid_t    uid_t;
typedef suki_gid_t    gid_t;
typedef suki_clockid_t clockid_t;
typedef suki_time_t   time_t;
typedef suki_dev_t    dev_t;
typedef suki_ino_t    ino_t;
typedef suki_nlink_t  nlink_t;
typedef long          blksize_t;
typedef long          blkcnt_t;

struct timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};
struct timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};
struct timezone {
    int32_t tz_minuteswest;
    int32_t tz_dsttime;
};
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
struct dirent {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    uint8_t  _pad[5];
    char     d_name[256];
};
typedef struct timeval  timeval;
typedef struct timespec timespec;
typedef struct stat     stat;
typedef struct utsname  utsname;
typedef struct tms      tms;
typedef struct rlimit   rlimit;
typedef struct dirent   dirent;

/* 把内核返回的 -errno 转成「设置 errno 并返回 -1」的标准 POSIX 语义 */
static inline long libc_ret(long r)
{
    if (r < 0 && r >= -4095) {
        errno = (int)(-r);
        return -1;
    }
    return r;
}

/* ---- 堆（sys_brk 后端） ---- */
void  *malloc(size_t size);
void  *calloc(size_t nmemb, size_t size);
void  *realloc(void *ptr, size_t size);
void   free(void *ptr);

/* ---- 字符串 ---- */
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *d, const char *s);
char  *strncpy(char *d, const char *s, size_t n);
size_t strlcpy(char *d, const char *s, size_t n);
char  *strcat(char *d, const char *s);
char  *strncat(char *d, const char *s, size_t n);
char  *strdup(const char *s);
char  *strndup(const char *s, size_t n);
/* 注意：strchr/strrchr 的底层实现在 suki.c（供 FatFs 等使用），声明见 suki.h，
 * 本头不再重复声明以避免重复定义。 */
char  *strrchr(const char *s, int c);
char  *strstr(const char *hay, const char *needle);
char  *strtok_r(char *s, const char *sep, char **save);
char  *strtok(char *s, const char *sep);
long   strtol(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
int    atoi(const char *s);
long   atol(const char *s);
int    isalpha(int c);
int    isdigit(int c);
int    isalnum(int c);
int    isspace(int c);
int    isupper(int c);
int    islower(int c);
int    isprint(int c);
int    toupper(int c);
int    tolower(int c);

/* ---- 标准输出/格式化 ---- */
int putchar(int c);
int puts(const char *s);
int printf(const char *fmt, ...);
int fprintf(int fd, const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

/* ---- 进程 / 文件（POSIX 名字，内核 fd 层后端） ---- */
int   open(const char *path, int flags, ...);
int   close(int fd);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
off_t lseek(int fd, off_t off, int whence);
int   unlink(const char *path);
int   mkdir(const char *path, int mode);
int   rename(const char *oldp, const char *newp);
int   access(const char *path, int mode);
int   chmod(const char *path, int mode);
int   chdir(const char *path);
char *getcwd(char *buf, size_t size);
int   dup(int fd);
int   dup2(int oldfd, int newfd);
int   pipe(int fds[2]);
int   fork(void);
int   waitpid(int pid, int *status, int options);
int   kill(int pid, int sig);
int   getpid(void);
int   getppid(void);
void  _exit(int code);
void  exit(int code);

/* ---- 时间 ---- */
time_t    time(time_t *tloc);
int       gettimeofday(struct timeval *tv, void *tz);
int       clock_gettime(int clk, struct timespec *tp);
int       nanosleep(const struct timespec *req, struct timespec *rem);
unsigned int sleep(unsigned int sec);
int       usleep(unsigned int usec);

/* ---- 系统 ---- */
int uname(struct utsname *buf);

/* 标准 errno 错误码名（映射到 <sukios/posix.h> 的 SUKI_E*，值一致） */
#define EPERM     SUKI_EPERM
#define ENOENT    SUKI_ENOENT
#define ESRCH     SUKI_ESRCH
#define EINTR     SUKI_EINTR
#define EIO       SUKI_EIO
#define ENXIO     SUKI_ENXIO
#define E2BIG     SUKI_E2BIG
#define ENOEXEC   SUKI_ENOEXEC
#define EBADF     SUKI_EBADF
#define ECHILD    SUKI_ECHILD
#define EAGAIN    SUKI_EAGAIN
#define ENOMEM    SUKI_ENOMEM
#define EACCES    SUKI_EACCES
#define EFAULT    SUKI_EFAULT
#define EBUSY     SUKI_EBUSY
#define EEXIST    SUKI_EEXIST
#define EXDEV     SUKI_EXDEV
#define ENODEV    SUKI_ENODEV
#define ENOTDIR   SUKI_ENOTDIR
#define EISDIR    SUKI_EISDIR
#define EINVAL    SUKI_EINVAL
#define ENFILE    SUKI_ENFILE
#define EMFILE    SUKI_EMFILE
#define ENOTTY    SUKI_ENOTTY
#define EFBIG     SUKI_EFBIG
#define ENOSPC    SUKI_ENOSPC
#define ESPIPE    SUKI_ESPIPE
#define EROFS     SUKI_EROFS
#define EMLINK    SUKI_EMLINK
#define EPIPE     SUKI_EPIPE
#define EDOM      SUKI_EDOM
#define ERANGE    SUKI_ERANGE
#define EDEADLK   SUKI_EDEADLK
#define ENAMETOOLONG SUKI_ENAMETOOLONG
#define ENOLCK    SUKI_ENOLCK
#define ENOSYS    SUKI_ENOSYS
#define ENOTEMPTY SUKI_ENOTEMPTY
#define ELOOP     SUKI_ELOOP
#define ENOSR     SUKI_ENOSR
#define EILSEQ    SUKI_EILSEQ
#define ENOTSOCK  SUKI_ENOTSOCK
#define EMSGSIZE  SUKI_EMSGSIZE
#define EPROTONOSUPPORT SUKI_EPROTONOSUPPORT
#define EOPNOTSUPP SUKI_EOPNOTSUPP
#define ECONNREFUSED SUKI_ECONNREFUSED
#define ETIMEDOUT SUKI_ETIMEDOUT
#define EHOSTUNREACH SUKI_EHOSTUNREACH
#define EALREADY  SUKI_EALREADY
#define EINPROGRESS SUKI_EINPROGRESS
#define ECANCELED SUKI_ECANCELED

/* 常量宏（POSIX 程序直接用） */
#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

#ifndef NULL
#define NULL ((void *)0)
#endif

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

#define CLOCK_REALTIME        SUKI_CLOCK_REALTIME
#define CLOCK_MONOTONIC       SUKI_CLOCK_MONOTONIC

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

#endif /* _SUKI_USER_LIBC_H */
