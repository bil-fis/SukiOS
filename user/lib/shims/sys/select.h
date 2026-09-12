/*
 * user/lib/shims/sys/select.h
 * -----------------------------------------------------------------------------
 * SukiOS 用户态标准 <sys/select.h>。fd_set 复用内核 suki_fd_set_t（1024 位）。
 * 实现见 user/lib/net.c（select 包装 -> SYS_SELECT）。
 */
#ifndef _SUKI_SHIM_SYS_SELECT_H
#define _SUKI_SHIM_SYS_SELECT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sukios/posix.h>
#include <sys/time.h>

typedef suki_fd_set_t fd_set;

#define FD_SETSIZE   SUKI_FD_SETSIZE

#define FD_ZERO(set)    memset((set), 0, sizeof(fd_set))
#define FD_SET(fd, set) do { int _f = (fd); \
    if (_f >= 0 && _f < FD_SETSIZE) (set)->bits[_f / 64] |= (1ULL << (_f % 64)); } while (0)
#define FD_CLR(fd, set) do { int _f = (fd); \
    if (_f >= 0 && _f < FD_SETSIZE) (set)->bits[_f / 64] &= ~(1ULL << (_f % 64)); } while (0)
#define FD_ISSET(fd, set) \
    (((fd) >= 0 && (fd) < FD_SETSIZE) ? (((set)->bits[(fd) / 64] >> ((fd) % 64)) & 1) : 0)

/* select 包装（实现见 user/lib/net.c） */
int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout);

#endif /* _SUKI_SHIM_SYS_SELECT_H */
