/*
 * user/lib/syscalls.c
 * newlib 风格的底层 syscall 桩（_read/_write/_open/_close/...），供本 libc
 * 内部复用，也作为后续直接编译 newlib 源码（lib/newlib-4.6.0.20260123）时的
 * syscall 后端对接点。每个 _xxx 直接映射到 SukiOS syscall 号（posix.h）。
 *
 * 注意：本文件与 unistd.c 的标准 C 名（read/write/open/...）并存——二者都
 * 是强符号但功能一致；newlib 仅链接 _xxx 版本，本 libc 的标准名供普通程序。
 */
#include "libc.h"
#include <stdarg.h>

int _open(const char *path, int flags, ...)
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

int _close(int fd)
{
    return close(fd);
}

int _read(int fd, char *buf, int len)
{
    long r = suki_syscall3(SYS_READ, (uint64_t)fd, (uint64_t)buf, (uint64_t)len);
    return (int)libc_ret(r);
}

int _write(int fd, const char *buf, int len)
{
    long r = suki_syscall3(SYS_WRITE, (uint64_t)fd, (uint64_t)buf, (uint64_t)len);
    return (int)libc_ret(r);
}

off_t _lseek(int fd, off_t off, int whence)
{
    return lseek(fd, off, whence);
}

int _fstat(int fd, struct stat *st)
{
    long r = suki_syscall3(SYS_FSTAT, (uint64_t)fd, (uint64_t)st, 0);
    return (int)libc_ret(r);
}

int _isatty(int fd)
{
    (void)fd;
    return 0;   /* SukiOS 当前无 TTY 概念，统一非 tty */
}

void *_sbrk(intptr_t incr)
{
    int64_t r = suki_syscall1(SYS_SBRK, (int64_t)incr);
    if (r < 0) { errno = ENOMEM; return (void *)-1; }
    return (void *)r;
}

/* _exit 由 stdlib.c 统一定义（与 exit 共享同一后端），此处不再重复定义，
 * 避免链接期 multiple definition。 */

int _fork(void)
{
    return fork();
}

int _execve(const char *path, char *const argv[], char *const envp[])
{
    long r = suki_syscall5(SYS_EXECVE, (uint64_t)path,
                           (uint64_t)argv, (uint64_t)envp, 0, 0);
    return (int)libc_ret(r);
}

int _stat(const char *path, struct stat *st)
{
    long r = suki_syscall2(SYS_STAT, (uint64_t)path, (uint64_t)st);
    return (int)libc_ret(r);
}

int _kill(int pid, int sig)
{
    return kill(pid, sig);
}

int _getpid(void)
{
    return getpid();
}

int _wait(int *status)
{
    return waitpid(-1, status, 0);
}

/* ========================================================================== */
/* 线程 / TLS / futex 底层 syscall 封装（pthread.c 复用）                       */
/* ========================================================================== */

long clone(unsigned long flags, void *child_stack, void *trampoline,
           void *tls, void *ptid, void *ctid)
{
    long r = suki_syscall6(SYS_CLONE, (uint64_t)flags, (uint64_t)child_stack,
                           (uint64_t)trampoline, (uint64_t)tls,
                           (uint64_t)ptid, (uint64_t)ctid);
    return r;
}

int futex(uint32_t *uaddr, int op, uint32_t val, const void *timeout,
          uint32_t *uaddr2, uint32_t val3)
{
    long r = suki_syscall6(SYS_FUTEX, (uint64_t)uaddr, (uint64_t)op,
                           (uint64_t)val, (uint64_t)timeout,
                           (uint64_t)uaddr2, (uint64_t)val3);
    /* futex 返回 0 表示成功（唤醒/睡眠），负值（内核 errno）需经 libc_ret 转换 */
    return (int)libc_ret(r);
}

long arch_prctl(int code, void *addr)
{
    long r = suki_syscall2(SYS_ARCH_PRCTL, (uint64_t)code, (uint64_t)addr);
    return (long)libc_ret(r);
}

int gettid(void)
{
    long r = suki_syscall0(SYS_GETTID);
    return (int)libc_ret(r);
}

int set_tid_address(int *tidptr)
{
    long r = suki_syscall1(SYS_SET_TID_ADDRESS, (uint64_t)tidptr);
    return (int)libc_ret(r);
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off)
{
    void *r = sys_mmap_posix(addr, len, prot, flags, fd, off);
    if ((intptr_t)r < 0) {
        errno = (int)(-(intptr_t)r);
        return MAP_FAILED;
    }
    return r;
}

int munmap(void *addr, size_t len)
{
    long r = suki_syscall2(SYS_MUNMAP, (uint64_t)addr, (uint64_t)len);
    return (int)libc_ret(r);
}
