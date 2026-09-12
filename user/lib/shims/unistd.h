/*
 * user/lib/shims/unistd.h
 * -----------------------------------------------------------------------------
 * SukiOS 用户态标准 <unistd.h> 实现头（freestanding libc 提供）。
 *
 * 实现归属：user/lib/unistd.c（经 suki.h 的 syscall 封装落到内核）。
 * 本文件提供 POSIX 文件/进程/目录接口的名字、类型与常量宏，数值全部取自
 * <sukios/posix.h>（唯一真相源）。
 */
#ifndef _SUKI_SHIM_UNISTD_H
#define _SUKI_SHIM_UNISTD_H

#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <sukios/posix.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

/* 基础类型（与内核/posix.h 一致）。
 * ssize_t 加守卫：<sys/socket.h> 亦需该类型，两处共用同一 typedef 以免重复定义。 */
#ifndef _SUKI_SHIM_SSIZE_T_DEFINED
#define _SUKI_SHIM_SSIZE_T_DEFINED
typedef int64_t  ssize_t;
#endif
typedef int64_t  off_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;
typedef int      pid_t;

/* 标准流文件号 */
#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

/* 文件访问模式（open 第三参/access 第二参） */
#define O_RDONLY    SUKI_O_RDONLY
#define O_WRONLY    SUKI_O_WRONLY
#define O_RDWR      SUKI_O_RDWR
#define O_CREAT     SUKI_O_CREAT
#define O_EXCL      SUKI_O_EXCL
#define O_TRUNC     SUKI_O_TRUNC
#define O_APPEND    SUKI_O_APPEND
#define O_NONBLOCK  SUKI_O_NONBLOCK
#define O_DIRECTORY SUKI_O_DIRECTORY

/* 文件偏移基准 */
#define SEEK_SET    SUKI_SEEK_SET
#define SEEK_CUR    SUKI_SEEK_CUR
#define SEEK_END    SUKI_SEEK_END

/* access() 模式 */
#define F_OK  SUKI_F_OK
#define R_OK  SUKI_R_OK
#define W_OK  SUKI_W_OK
#define X_OK  SUKI_X_OK

/* waitpid() 选项 */
#define WNOHANG  SUKI_WNOHANG

/* ---- 文件 I/O（user/lib/unistd.c） ---- */
int   open(const char *path, int flags, ...);
int   close(int fd);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
off_t lseek(int fd, off_t offset, int whence);
int   unlink(const char *path);
int   mkdir(const char *path, int mode);
int   rename(const char *oldpath, const char *newpath);
int   access(const char *path, int mode);
int   chmod(const char *path, int mode);
int   chdir(const char *path);
char *getcwd(char *buf, size_t size);
int   fsync(int fd);

/* ---- 进程 / 管道 ---- */
int   dup(int fd);
int   dup2(int oldfd, int newfd);
int   pipe(int fds[2]);
int   fork(void);
int   waitpid(int pid, int *status, int options);
int   kill(int pid, int sig);
int   getpid(void);
int   getppid(void);
void  _exit(int code);
unsigned int sleep(unsigned int seconds);
int   usleep(unsigned int usec);

/* 把内核返回的 -errno 转成「设置 errno 并返回 -1」的标准 POSIX 语义 */
static inline long __libc_ret(long r)
{
    if (r < 0 && r >= -4095) { errno = (int)(-r); return -1; }
    return r;
}

#endif /* _SUKI_SHIM_UNISTD_H */
