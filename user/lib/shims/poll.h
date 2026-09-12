/*
 * user/lib/shims/poll.h
 * -----------------------------------------------------------------------------
 * SukiOS 用户态标准 <poll.h>。struct pollfd 布局与内核 suki_pollfd_t 一致
 * （int fd + short events + short revents = 8 字节），可直接传给 SYS_POLL。
 */
#ifndef _SUKI_SHIM_POLL_H
#define _SUKI_SHIM_POLL_H

#include <stdint.h>
#include <sukios/posix.h>

struct pollfd {
    int   fd;       /* 文件/socket 描述符 */
    short events;   /* 请求的事件 */
    short revents;  /* 返回的事件 */
};

#define POLLIN    SUKI_POLLIN
#define POLLPRI   SUKI_POLLPRI
#define POLLOUT   SUKI_POLLOUT
#define POLLERR   SUKI_POLLERR
#define POLLHUP   SUKI_POLLHUP
#define POLLNVAL  SUKI_POLLNVAL

/* poll 包装（实现见 user/lib/net.c） */
int poll(struct pollfd *fds, unsigned int nfds, int timeout);

#endif /* _SUKI_SHIM_POLL_H */
