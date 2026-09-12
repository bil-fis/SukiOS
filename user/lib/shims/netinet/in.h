/*
 * user/lib/shims/netinet/in.h
 * -----------------------------------------------------------------------------
 * SukiOS 用户态标准 <netinet/in.h>。
 *
 * 关键约定（与 net_server 的 suki_sockaddr_in 完全一致，见 include/sukios/net.h）：
 *   - sin_port 为【主机字节序】（SukiOS socket ABI 在 syscall 边界统一主机序，
 *     实测 nettest 以 sin_port=69（主机序）成功收发 UDP，故 htons/ntohs 为恒等）。
 *   - sin_addr.s_addr 为 4 字节八位组（a.b.c.d），主机序。
 */
#ifndef _SUKI_SHIM_NETINET_IN_H
#define _SUKI_SHIM_NETINET_IN_H

#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

struct in_addr {
    uint8_t s_addr[4];   /* a.b.c.d（主机序八位组） */
};

struct sockaddr_in {
    sa_family_t sin_family;   /* AF_INET */
    in_port_t   sin_port;     /* 主机字节序 */
    struct in_addr sin_addr;
    uint8_t     sin_zero[8];
};

/* 字节序转换：SukiOS socket ABI 主机序，故 htons/ntohs/htonl/ntohl 均为恒等。
 * 调用方（如 libcurl）写 htons(port) 后得到的仍是主机序，与 ABI 一致。 */
#define htons(x) ((uint16_t)(x))
#define ntohs(x) ((uint16_t)(x))
#define htonl(x) ((uint32_t)(x))
#define ntohl(x) ((uint32_t)(x))

#define INADDR_ANY        ((in_addr_t)0)
#define INADDR_LOOPBACK   ((in_addr_t)0x0100007F)  /* 127.0.0.1 */
#define INADDR_BROADCAST  ((in_addr_t)0xFFFFFFFF)
#define INADDR_NONE       ((in_addr_t)0xFFFFFFFF)

#define INET_ADDRSTRLEN   16
#define INET6_ADDRSTRLEN  46

#endif /* _SUKI_SHIM_NETINET_IN_H */
