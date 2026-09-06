/*
 * include/sukios/net.h
 * -----------------------------------------------------------------------------
 * SukiOS 网络子系统公共契约（内核 / 用户态 / net_server 三者共享，单一真相源）。
 *
 * 两层接口共用同一 NS_PORT(Mach IPC) 协议：
 *   1) POSIX 网络 syscall（150..169）：内核 sys_net_dispatch 经 net_rpc 转发 NS_PORT；
 *   2) SukiNative 原生 socket API（suki_socket_*）：用户态 libsuki 直接 mach_msg 到
 *      NS_PORT（不经过内核 syscall，符合 SukiNative「一切皆对象/IPC」模型）。
 *
 * 协议（与 disk_proto.h / net_proto.h 同构）：请求 = mach_msg_header + sock_req_t
 * (+ 可选内联 data)；应答 = mach_msg_header + sock_resp_t (+ 可选内联 data)。
 * 全部走内联小消息（< MACH_MSG_INLINE_MAX），故 SOCK_MAX_DATA 须保证整条消息
 * 不超 3968 字节上限。
 *
 * 调用关系：kernel/net/socket.c（转发） + user/lib/suki_native_net.c（原生封装）
 *           -> NS_PORT -> user/net_server.c（lwIP 协议栈，NO_SYS=1 raw API）。
 */
#ifndef _SUKI_SUKIOS_NET_H
#define _SUKI_SUKIOS_NET_H

#include <stdint.h>
#include <stddef.h>

/* ---- 地址族（POSIX 兼容子集）---- */
#define SUKI_AF_UNSPEC   0
#define SUKI_AF_INET     2      /* IPv4 */
#define SUKI_AF_INET6    10     /* 暂未实现，保留 */

/* ---- socket 类型 ---- */
#define SUKI_SOCK_STREAM 1      /* TCP */
#define SUKI_SOCK_DGRAM  2      /* UDP */
#define SUKI_SOCK_RAW    3      /* 暂未实现 */

/* ---- 协议号（IPPROTO_*）---- */
#define SUKI_IPPROTO_IP   0
#define SUKI_IPPROTO_TCP  6
#define SUKI_IPPROTO_UDP  17

/* ---- NS_PORT 消息 ID（区别于 NET_PORT 的 NET_MSG_*）---- */
#define SOCK_MSG_CREATE       1
#define SOCK_MSG_BIND         2
#define SOCK_MSG_CONNECT      3
#define SOCK_MSG_LISTEN       4
#define SOCK_MSG_ACCEPT       5
#define SOCK_MSG_SEND         6
#define SOCK_MSG_RECV         7
#define SOCK_MSG_CLOSE        8
#define SOCK_MSG_GETSOCKNAME  9
#define SOCK_MSG_GETPEERNAME  10
#define SOCK_MSG_SETSOCKOPT   11
#define SOCK_MSG_GETSOCKOPT   12
#define SOCK_MSG_SHUTDOWN     13
#define SOCK_MSG_SENDTO       14
#define SOCK_MSG_RECVFROM     15

/* 内联数据上限（含请求/应答头仍须 < 3968） */
#define SOCK_MAX_DATA   3584

/* 地址编码（内核与 net_server 内部 ABI，主机字节序，避免 ntohl 困扰）：
 *   sin_port 为主机序端口；sin_addr 为 4 个 IPv4 八位组（a.b.c.d）按主机序拼成
 *   的 uint32（ip[0]=a, ip[1]=b, ip[2]=c, ip[3]=d）。net_server 再用
 *   IP4_ADDR(ipaddr, ip[0], ip[1], ip[2], ip[3]) 转换，零歧义。
 * 用户态 POSIX 测试程序直接以本结构体作为 struct sockaddr_in 使用（布局一致）。 */
typedef struct suki_sockaddr_in {
    uint16_t sin_family;        /* SUKI_AF_INET */
    uint16_t sin_port;          /* 主机字节序 */
    uint8_t  sin_addr[4];       /* a.b.c.d（主机序八位组） */
    uint8_t  sin_zero[8];
} suki_sockaddr_in_t;

/* 通用地址头兼容性包装（POSIX bind/connect 接收 struct sockaddr*） */
typedef struct suki_sockaddr {
    uint16_t sa_family;
    uint8_t  sa_data[14];
} suki_sockaddr_t;

/* ---- 请求体（紧跟 mach_msg_header）---- */
typedef struct sock_req {
    uint32_t op;                /* SOCK_MSG_* */
    uint32_t sock;              /* socket 句柄（CREATE 时为 0） */
    uint32_t flags;             /* recv/send 标志位（MSG_*，暂仅 0/DONTWAIT） */
    uint32_t domain;            /* CREATE: SUKI_AF_* */
    uint32_t type;              /* CREATE: SUKI_SOCK_* */
    uint32_t proto;             /* CREATE: SUKI_IPPROTO_* */
    uint32_t addr_len;          /* 地址长度（bind/connect/recvfrom） */
    uint8_t  addr[16];          /* 地址（suki_sockaddr_in 布局，16 字节） */
    uint32_t len;               /* 数据长度（send/sendto 请求；recv 请求时的期望上限） */
    uint8_t  data[SOCK_MAX_DATA];
} sock_req_t;

/* ---- 应答体（紧跟 mach_msg_header）---- */
typedef struct sock_resp {
    uint32_t status;            /* 0=成功，<0=负 errno（如 -SUKI_EAGAIN） */
    uint32_t result;            /* 操作相关：ACCEPT/新 socket 句柄、RECV 实际字节数、0 */
    uint32_t addr_len;          /* 回填地址长度（recvfrom/accept/getsockname/getpeername） */
    uint8_t  addr[16];          /* 回填地址（suki_sockaddr_in 布局） */
    uint32_t len;               /* 数据长度（recv/recvfrom 应答） */
    uint8_t  data[SOCK_MAX_DATA];
} sock_resp_t;

/* 服务下线占位应答（net_server 退出时由内核排干逻辑处理，正常路径不会收到） */
#define SOCK_MSG_SERVICE_DOWN  0xFFFF

/* ============================================================================
 *  SukiNative 原生 socket API（用户态 libsuki 封装，直接 IPC 到 NS_PORT）
 *  一切皆对象，返回 suki_status_t 状态码（0=成功，<0=SUKI_E*）。与 POSIX 平行，
 *  新服务可直接使用。
 * ========================================================================== */
typedef int64_t suki_status_t;
typedef uint32_t suki_socket_t;      /* socket 句柄（net_server 分配） */

#ifdef __cplusplus
extern "C" {
#endif

suki_status_t suki_socket_create(int domain, int type, int proto,
                                 suki_socket_t *out);
suki_status_t suki_socket_bind(suki_socket_t s, const void *addr,
                                uint32_t addrlen);
suki_status_t suki_socket_connect(suki_socket_t s, const void *addr,
                                   uint32_t addrlen);
suki_status_t suki_socket_listen(suki_socket_t s, int backlog);
suki_status_t suki_socket_accept(suki_socket_t s, void *addr,
                                  uint32_t *addrlen, suki_socket_t *out);
suki_status_t suki_socket_send(suki_socket_t s, const void *buf, uint32_t len,
                                uint32_t flags, uint32_t *out_nwritten);
suki_status_t suki_socket_recv(suki_socket_t s, void *buf, uint32_t len,
                                uint32_t flags, uint32_t *out_nread);
suki_status_t suki_socket_sendto(suki_socket_t s, const void *buf, uint32_t len,
                                  uint32_t flags, const void *addr,
                                  uint32_t addrlen, uint32_t *out_nwritten);
suki_status_t suki_socket_recvfrom(suki_socket_t s, void *buf, uint32_t len,
                                    uint32_t flags, void *addr,
                                    uint32_t *addrlen, uint32_t *out_nread);
suki_status_t suki_socket_close(suki_socket_t s);
suki_status_t suki_socket_getsockname(suki_socket_t s, void *addr,
                                       uint32_t *addrlen);
suki_status_t suki_socket_getpeername(suki_socket_t s, void *addr,
                                       uint32_t *addrlen);
suki_status_t suki_socket_shutdown(suki_socket_t s, int how);
suki_status_t suki_socket_setsockopt(suki_socket_t s, int level, int optname,
                                      const void *optval, uint32_t optlen);
suki_status_t suki_socket_getsockopt(suki_socket_t s, int level, int optname,
                                      void *optval, uint32_t *optlen);

#ifdef __cplusplus
}
#endif

#endif /* _SUKI_SUKIOS_NET_H */
