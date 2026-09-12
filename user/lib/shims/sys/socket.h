/*
 * user/lib/shims/sys/socket.h
 * -----------------------------------------------------------------------------
 * SukiOS 用户态标准 <sys/socket.h>（BSD socket API 声明 + 类型 + 常量）。
 *
 * 所有常量与内核 ABI 一致：值取自 <sukios/posix.h>（SUKI_AF_ 系列 / SUKI_SOCK_ 系列
 * / SUKI_MSG_ 系列），保证 libc 包装器与内核 sys_net_dispatch 完全同构。sockaddr_in
 * 布局（主机序端口与 4 字节八位组地址）由 netinet/in.h 定义，与 net_server 的
 * suki_sockaddr_in 一致。
 */
#ifndef _SUKI_SHIM_SYS_SOCKET_H
#define _SUKI_SHIM_SYS_SOCKET_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/select.h>   /* POSIX：<sys/socket.h> 提供 fd_set/FD_* 与 select() */
#include <sukios/posix.h>

/* ---- 基础类型 ---- */
/* ssize_t / socklen_t / sa_family_t 由 <sys/types.h> 提供（守卫宏避免重复定义）。 */

struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

/* ---- 地址族 ---- */
#define AF_UNSPEC  0
#define AF_UNIX    SUKI_AF_UNIX
#define AF_INET    SUKI_AF_INET
#define AF_INET6   SUKI_AF_INET6

/* ---- 协议族（与地址族同值；POSIX 定义 PF_* 为 AF_* 别名） ---- */
#define PF_UNSPEC  AF_UNSPEC
#define PF_UNIX    AF_UNIX
#define PF_LOCAL   AF_UNIX
#define PF_INET    AF_INET
#define PF_INET6   AF_INET6

/* ---- socket 类型 ---- */
#define SOCK_STREAM SUKI_SOCK_STREAM
#define SOCK_DGRAM  SUKI_SOCK_DGRAM
#define SOCK_RAW    SUKI_SOCK_RAW

/* ---- 消息标志 ---- */
#define MSG_OOB       SUKI_MSG_OOB
#define MSG_PEEK      SUKI_MSG_PEEK
#define MSG_DONTROUTE SUKI_MSG_DONTROUTE
#define MSG_CTRUNC    SUKI_MSG_CTRUNC
#define MSG_TRUNC     SUKI_MSG_TRUNC
#define MSG_WAITALL   SUKI_MSG_WAITALL
#define MSG_DONTWAIT  SUKI_MSG_DONTWAIT
#define MSG_EOR       SUKI_MSG_EOR
#define MSG_NOSIGNAL  SUKI_MSG_NOSIGNAL

/* ---- 协议层 / 协议号（字面量，与内核 SUKI_IPPROTO_* 一致） ---- */
#define SOL_SOCKET   0x1
#define IPPROTO_IP    0
#define IPPROTO_ICMP  1
#define IPPROTO_TCP   6
#define IPPROTO_UDP   17

/* ---- socket 选项 ---- */
#define SO_DEBUG      SUKI_SO_DEBUG
#define SO_REUSEADDR  SUKI_SO_REUSEADDR
#define SO_TYPE       SUKI_SO_TYPE
#define SO_ERROR      SUKI_SO_ERROR
#define SO_DONTROUTE  SUKI_SO_DONTROUTE
#define SO_BROADCAST  SUKI_SO_BROADCAST
#define SO_SNDBUF     SUKI_SO_SNDBUF
#define SO_RCVBUF     SUKI_SO_RCVBUF
#define SO_KEEPALIVE  SUKI_SO_KEEPALIVE
#define SO_LINGER     SUKI_SO_LINGER
#define SO_RCVTIMEO   SUKI_SO_RCVTIMEO
#define SO_SNDTIMEO   SUKI_SO_SNDTIMEO
#define SO_ACCEPTCONN SUKI_SO_ACCEPTCONN

#define IP_TOS      SUKI_IP_TOS
#define IP_TTL      SUKI_IP_TTL
#define TCP_NODELAY SUKI_TCP_NODELAY

/* ---- shutdown 方式 ---- */
#define SHUT_RD   SUKI_SHUT_RD
#define SHUT_WR   SUKI_SHUT_WR
#define SHUT_RDWR SUKI_SHUT_RDWR

/* ---- 聚合 I/O 结构 ---- */
struct iovec {
    void   *iov_base;
    size_t  iov_len;
};

struct msghdr {
    void         *msg_name;
    socklen_t     msg_namelen;
    struct iovec *msg_iov;
    size_t        msg_iovlen;
    void         *msg_control;
    size_t        msg_controllen;
    int           msg_flags;
};

/* ---- BSD socket 函数原型（实现见 user/lib/net.c） ---- */
int    socket(int domain, int type, int protocol);
int    bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int    connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int    listen(int sockfd, int backlog);
int    accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int    accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags);
ssize_t send(int sockfd, const void *buf, size_t len, int flags);
ssize_t recv(int sockfd, void *buf, size_t len, int flags);
ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen);
ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen);
int    sendmsg(int sockfd, const struct msghdr *msg, int flags);
int    recvmsg(int sockfd, struct msghdr *msg, int flags);
int    setsockopt(int sockfd, int level, int optname,
                  const void *optval, socklen_t optlen);
int    getsockopt(int sockfd, int level, int optname,
                  void *optval, socklen_t *optlen);
int    getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int    getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int    shutdown(int sockfd, int how);
int    socketpair(int domain, int type, int protocol, int sv[2]);

#endif /* _SUKI_SHIM_SYS_SOCKET_H */
