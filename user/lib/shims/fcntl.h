/*
 * user/lib/shims/fcntl.h
 * -----------------------------------------------------------------------------
 * SukiOS 用户态标准 <fcntl.h>。fcntl 包装实现见 user/lib/net.c（SYS_FCNTL）。
 * open 标志复用 libc.h 已定义的 SUKI_O_*（O_RDONLY 等），此处统一可经 fcntl 设置。
 */
#ifndef _SUKI_SHIM_FCNTL_H
#define _SUKI_SHIM_FCNTL_H

#include <sukios/posix.h>

/* fcntl 命令 */
#define F_DUPFD    SUKI_F_DUPFD
#define F_GETFD    SUKI_F_GETFD
#define F_SETFD    SUKI_F_SETFD
#define F_GETFL    SUKI_F_GETFL
#define F_SETFL    SUKI_F_SETFL
#define F_GETLK    SUKI_F_GETLK
#define F_SETLK    SUKI_F_SETLK
#define F_SETLKW   SUKI_F_SETLKW
#define FD_CLOEXEC SUKI_FD_CLOEXEC

/* 文件状态标志（O_*，与 libc.h 映射一致） */
#ifndef O_RDONLY
#define O_RDONLY   SUKI_O_RDONLY
#endif
#ifndef O_WRONLY
#define O_WRONLY   SUKI_O_WRONLY
#endif
#ifndef O_RDWR
#define O_RDWR     SUKI_O_RDWR
#endif
#ifndef O_CREAT
#define O_CREAT    SUKI_O_CREAT
#endif
#ifndef O_EXCL
#define O_EXCL     SUKI_O_EXCL
#endif
#ifndef O_TRUNC
#define O_TRUNC    SUKI_O_TRUNC
#endif
#ifndef O_APPEND
#define O_APPEND   SUKI_O_APPEND
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK SUKI_O_NONBLOCK
#endif

/* fcntl 包装（实现见 user/lib/net.c） */
int fcntl(int fd, int cmd, ...);   /* 标准可变参数：F_GETFL 等无第三参，F_SETFL 带 flags */

#endif /* _SUKI_SHIM_FCNTL_H */
