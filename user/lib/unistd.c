/*
 * user/lib/unistd.c
 * POSIX 进程/文件操作包装（内核 fd 层 + sys_* 调用后端）。
 * 所有函数遵循标准 POSIX 语义：失败设 errno 并返回 -1（或约定值）。
 */
#include "libc.h"
#include <stdarg.h>

int open(const char *path, int flags, ...)
{
    uint32_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode = (uint32_t)va_arg(ap, unsigned int);
        va_end(ap);
    }
    long r = suki_syscall3(SYS_OPEN, (uint64_t)path, (uint64_t)flags, (uint64_t)mode);
    return (int)libc_ret(r);
}

int close(int fd)
{
    long r = suki_syscall1(SYS_CLOSE, (uint64_t)fd);
    return (int)libc_ret(r);
}

ssize_t read(int fd, void *buf, size_t count)
{
    long r = suki_syscall3(SYS_READ, (uint64_t)fd, (uint64_t)buf, (uint64_t)count);
    return (ssize_t)libc_ret(r);
}

ssize_t write(int fd, const void *buf, size_t count)
{
    long r = suki_syscall3(SYS_WRITE, (uint64_t)fd, (uint64_t)buf, (uint64_t)count);
    return (ssize_t)libc_ret(r);
}

off_t lseek(int fd, off_t off, int whence)
{
    long r = suki_syscall3(SYS_LSEEK, (uint64_t)fd, (int64_t)off, (uint64_t)whence);
    return (off_t)libc_ret(r);
}

int unlink(const char *path)
{
    long r = suki_syscall1(SYS_UNLINK, (uint64_t)path);
    return (int)libc_ret(r);
}

int mkdir(const char *path, mode_t mode)
{
    long r = suki_syscall2(SYS_MKDIR, (uint64_t)path, (uint64_t)mode);
    return (int)libc_ret(r);
}

int rename(const char *oldp, const char *newp)
{
    long r = suki_syscall2(SYS_RENAME, (uint64_t)oldp, (uint64_t)newp);
    return (int)libc_ret(r);
}

int access(const char *path, int mode)
{
    long r = suki_syscall2(SYS_ACCESS, (uint64_t)path, (uint64_t)mode);
    return (int)libc_ret(r);
}

int chmod(const char *path, mode_t mode)
{
    long r = suki_syscall2(SYS_CHMOD, (uint64_t)path, (uint64_t)mode);
    return (int)libc_ret(r);
}

/* 文件元数据（供 libcurl 等第三方库使用）。suki_stat_t 布局与本 struct stat 一致。 */
int stat(const char *path, struct stat *buf)
{
    long r = suki_syscall2(SYS_STAT, (uint64_t)path, (uint64_t)buf);
    return (int)libc_ret(r);
}

int fstat(int fd, struct stat *buf)
{
    long r = suki_syscall2(SYS_FSTAT, (uint64_t)fd, (uint64_t)buf);
    return (int)libc_ret(r);
}

int lstat(const char *path, struct stat *buf)
{
    long r = suki_syscall2(SYS_LSTAT, (uint64_t)path, (uint64_t)buf);
    return (int)libc_ret(r);
}

int chdir(const char *path)
{
    long r = suki_syscall1(SYS_CHDIR, (uint64_t)path);
    return (int)libc_ret(r);
}

char *getcwd(char *buf, size_t size)
{
    long r = suki_syscall2(SYS_GETCWD, (uint64_t)buf, (uint64_t)size);
    if (libc_ret(r) < 0) return NULL;
    return buf;
}

int dup(int fd)
{
    long r = suki_syscall1(SYS_DUP, (uint64_t)fd);
    return (int)libc_ret(r);
}

int dup2(int oldfd, int newfd)
{
    long r = suki_syscall2(SYS_DUP2, (uint64_t)oldfd, (uint64_t)newfd);
    return (int)libc_ret(r);
}

int pipe(int fds[2])
{
    long r = suki_syscall1(SYS_PIPE, (uint64_t)fds);
    return (int)libc_ret(r);
}

int fork(void)
{
    long r = suki_syscall0(SYS_FORK);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (int)r;   /* 父返回子 pid；子返回 0 */
}

int waitpid(int pid, int *status, int options)
{
    long r = suki_syscall3(SYS_WAITPID, (uint64_t)pid,
                           (uint64_t)status, (uint64_t)options);
    if (r < 0 && r >= -4095) { errno = (int)(-r); return -1; }
    return (int)r;
}

int kill(int pid, int sig)
{
    long r = suki_syscall2(SYS_KILL, (uint64_t)pid, (uint64_t)sig);
    return (int)libc_ret(r);
}

int getpid(void)
{
    return (int)suki_syscall0(SYS_GETPID);
}

int getppid(void)
{
    return (int)suki_syscall0(SYS_GETPPID);
}

/* 主机名（供 libcurl 等第三方库使用）。内核 SYS_GETHOSTNAME(115) 未实现时
 * 回退固定名 "sukios"，保证接口始终可用。 */
int gethostname(char *name, size_t len)
{
    if (!name || len == 0) return -1;
    long r = suki_syscall2(SYS_GETHOSTNAME, (uint64_t)name, (uint64_t)len);
    if (r < 0) {
        const char *def = "sukios";
        size_t i = 0;
        while (def[i] && i + 1 < len) { name[i] = def[i]; i++; }
        name[i] = '\0';
    }
    return 0;
}

/* 截断/扩展文件到指定长度（供 curl 工具等使用）。 */
int ftruncate(int fd, off_t length)
{
    long r = suki_syscall2(SYS_FTRUNCATE, (uint64_t)fd, (uint64_t)length);
    return (int)libc_ret(r);
}

/* 是否为终端设备。SukiOS 用户态标准流走 CGA/串口，不是 POSIX tty；
 * 返回 0 使第三方程序（如 curl）走非终端分支（不绘制进度条、不报终端告警）。 */
int isatty(int fd)
{
    (void)fd;
    return 0;
}
