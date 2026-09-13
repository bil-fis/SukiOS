/*
 * user/lib/net.c
 * =============================================================================
 * SukiOS 用户态 BSD socket / 网络支持库实现。
 *
 * 设计要点：
 *   - 所有 BSD socket 函数直接包装内核 POSIX socket 系统调用（号位 150..166）。
 *     内核 sys_net_dispatch 已将其翻译为 SOCK_MSG_* 经 NS_PORT 发给 net_server
 *     （lwIP）。ABI（参数顺序、sockaddr 布局、addrlen 指针回写）与 kernel/net/socket.c
 *     严格一致。
 *   - sockaddr_in：sin_port 主机序、sin_addr 为 4 字节八位组（与 net_server 一致）。
 *   - htons/ntohs/htonl/ntohl 为恒等宏（见 <netinet/in.h> 说明）。
 *   - 提供 inet_pton/inet_ntop/inet_addr（数字地址）+ getaddrinfo DNS-over-UDP 解析器
 *     （默认 DNS 服务器 10.0.2.3，可由环境变量 SUKI_DNS_SERVER 覆盖）。
 *   - select/poll/fcntl 包装对应内核系统调用 SYS_SELECT/SYS_POLL/SYS_FCNTL。
 * =============================================================================
 */
#include "lib/suki.h"
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/time.h>

extern int errno;

/* 内核返回负 errno（two's complement）-> 设置 errno 并返回 -1。 */
static int net_ret(long r) {
    if (r < 0 && r >= -4095) { errno = (int)(-r); return -1; }
    return (int)r;
}

/* ===========================================================================
 * BSD socket 函数（包装内核 150..166 号位）
 * =========================================================================== */

int socket(int domain, int type, int protocol) {
    return net_ret((long)suki_syscall5(SYS_SOCKET, (uint64_t)domain, (uint64_t)type,
                                       (uint64_t)protocol, 0, 0));
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    return net_ret((long)suki_syscall5(SYS_BIND, (uint64_t)sockfd, (uint64_t)addr,
                                       (uint64_t)addrlen, 0, 0));
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    return net_ret((long)suki_syscall5(SYS_CONNECT, (uint64_t)sockfd, (uint64_t)addr,
                                       (uint64_t)addrlen, 0, 0));
}

int listen(int sockfd, int backlog) {
    return net_ret((long)suki_syscall5(SYS_LISTEN, (uint64_t)sockfd, (uint64_t)backlog,
                                       0, 0, 0));
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    socklen_t al = addrlen ? *addrlen : 0;
    long r = (long)suki_syscall5(SYS_ACCEPT, (uint64_t)sockfd, (uint64_t)addr,
                                 (uint64_t)&al, 0, 0);
    if (r < 0 && r >= -4095) { errno = (int)(-r); return -1; }
    if (addrlen) *addrlen = al;
    return (int)r;
}

int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags) {
    (void)flags; /* SukiOS 暂不支持 SOCK_NONBLOCK 等 accept4 专属标志，等价于 accept */
    return accept(sockfd, addr, addrlen);
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    return (ssize_t)net_ret((long)suki_syscall5(SYS_SEND, (uint64_t)sockfd, (uint64_t)buf,
                                                (uint64_t)len, (uint64_t)flags, 0));
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    return (ssize_t)net_ret((long)suki_syscall5(SYS_RECV, (uint64_t)sockfd, (uint64_t)buf,
                                                (uint64_t)len, (uint64_t)flags, 0));
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen) {
    return (ssize_t)net_ret((long)suki_syscall6(SYS_SENDTO, (uint64_t)sockfd, (uint64_t)buf,
                                                (uint64_t)len, (uint64_t)flags,
                                                (uint64_t)dest_addr, (uint64_t)addrlen));
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen) {
    socklen_t al = src_addr ? (addrlen ? *addrlen : 0) : 0;
    long r = (long)suki_syscall6(SYS_RECVFROM, (uint64_t)sockfd, (uint64_t)buf,
                                 (uint64_t)len, (uint64_t)flags, (uint64_t)src_addr,
                                 (uint64_t)&al);
    if (r < 0 && r >= -4095) { errno = (int)(-r); return -1; }
    if (src_addr && addrlen) *addrlen = al;
    return (ssize_t)r;
}

int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen) {
    return net_ret((long)suki_syscall5(SYS_SETSOCKOPT, (uint64_t)sockfd, (uint64_t)level,
                                       (uint64_t)optname, (uint64_t)optval, (uint64_t)optlen));
}

int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen) {
    return net_ret((long)suki_syscall5(SYS_GETSOCKOPT, (uint64_t)sockfd, (uint64_t)level,
                                       (uint64_t)optname, (uint64_t)optval, (uint64_t)optlen));
}

int getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    socklen_t al = addrlen ? *addrlen : 0;
    long r = (long)suki_syscall5(SYS_GETPEERNAME, (uint64_t)sockfd, (uint64_t)addr,
                                 (uint64_t)&al, 0, 0);
    if (r < 0 && r >= -4095) { errno = (int)(-r); return -1; }
    if (addrlen) *addrlen = al;
    return 0;
}

int getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    socklen_t al = addrlen ? *addrlen : 0;
    long r = (long)suki_syscall5(SYS_GETSOCKNAME, (uint64_t)sockfd, (uint64_t)addr,
                                 (uint64_t)&al, 0, 0);
    if (r < 0 && r >= -4095) { errno = (int)(-r); return -1; }
    if (addrlen) *addrlen = al;
    return 0;
}

int shutdown(int sockfd, int how) {
    return net_ret((long)suki_syscall5(SYS_SHUTDOWN, (uint64_t)sockfd, (uint64_t)how, 0, 0, 0));
}

int socketpair(int domain, int type, int protocol, int sv[2]) {
    (void)domain; (void)type; (void)protocol; (void)sv;
    errno = EOPNOTSUPP;
    return -1;
}

/* sendmsg/recvmsg：仅实现单 iovec（覆盖 libcurl 典型用法）。 */
int sendmsg(int sockfd, const struct msghdr *msg, int flags) {
    if (!msg || msg->msg_iovlen < 1) { errno = EINVAL; return -1; }
    ssize_t n = sendto(sockfd, msg->msg_iov[0].iov_base, msg->msg_iov[0].iov_len, flags,
                       (const struct sockaddr *)msg->msg_name, msg->msg_namelen);
    return (int)n;
}

int recvmsg(int sockfd, struct msghdr *msg, int flags) {
    if (!msg || msg->msg_iovlen < 1) { errno = EINVAL; return -1; }
    ssize_t n = recvfrom(sockfd, msg->msg_iov[0].iov_base, msg->msg_iov[0].iov_len, flags,
                         msg->msg_name, &msg->msg_namelen);
    msg->msg_flags = 0;
    return (int)n;
}

/* ===========================================================================
 * select / poll / fcntl 包装
 * =========================================================================== */

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout) {
    return net_ret((long)suki_syscall5(SYS_SELECT, (uint64_t)nfds, (uint64_t)readfds,
                                       (uint64_t)writefds, (uint64_t)exceptfds,
                                       (uint64_t)timeout));
}

int poll(struct pollfd *fds, unsigned int nfds, int timeout) {
    return net_ret((long)suki_syscall5(SYS_POLL, (uint64_t)fds, (uint64_t)nfds,
                                       (uint64_t)timeout, 0, 0));
}

int fcntl(int fd, int cmd, ...) {
    /* 标准 fcntl 为可变参数：F_GETFL/F_GETFD 等无第三参，F_SETFL/F_SETFD 带 flags。
     * 统一读取一个 int 实参（无第三参时该值被内核在该 cmd 下忽略）。 */
    va_list ap;
    va_start(ap, cmd);
    int arg = va_arg(ap, int);
    va_end(ap);
    return net_ret((long)suki_syscall5(SYS_FCNTL, (uint64_t)fd, (uint64_t)cmd,
                                       (uint64_t)arg, 0, 0));
}

/* ===========================================================================
 * 地址转换
 * =========================================================================== */

int inet_pton(int af, const char *src, void *dst) {
    if (af == AF_INET) {
        const char *p = src;
        unsigned a[4];
        for (int i = 0; i < 4; i++) {
            if (!(*p >= '0' && *p <= '9')) return 0;
            unsigned v = 0;
            while (*p >= '0' && *p <= '9') {
                v = v * 10 + (unsigned)(*p - '0');
                if (v > 255) return 0;
                p++;
            }
            a[i] = v;
            if (i < 3) {
                if (*p != '.') return 0;
                p++;
            }
        }
        if (*p != 0) return 0;
        unsigned char *o = (unsigned char *)dst;
        o[0] = (unsigned char)a[0];
        o[1] = (unsigned char)a[1];
        o[2] = (unsigned char)a[2];
        o[3] = (unsigned char)a[3];
        return 1;
    }
    /* IPv6 未实现（libcurl 默认 CURL_DISABLE_IPV6） */
    (void)af; (void)src; (void)dst;
    return -1;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) {
    if (af == AF_INET) {
        const unsigned char *o = (const unsigned char *)src;
        if (size < (socklen_t)INET_ADDRSTRLEN) return NULL;
        int n = snprintf(dst, size, "%u.%u.%u.%u", o[0], o[1], o[2], o[3]);
        if (n < 0 || n >= (int)size) return NULL;
        return dst;
    }
    (void)af; (void)src; (void)dst; (void)size;
    return NULL;
}

in_addr_t inet_addr(const char *cp) {
    unsigned char o[4];
    if (inet_pton(AF_INET, cp, o) != 1) return INADDR_NONE;
    return (in_addr_t)((uint32_t)o[0] | ((uint32_t)o[1] << 8) |
                       ((uint32_t)o[2] << 16) | ((uint32_t)o[3] << 24));
}

/* ===========================================================================
 * 服务名/端口解析（getaddrinfo 用）
 * =========================================================================== */

static int parse_service(const char *service) {
    if (!service) return 0;
    const char *p = service;
    int v = 0, digits = 0;
    while (*p) {
        if (*p < '0' || *p > '9') { digits = -1; break; }
        v = v * 10 + (*p - '0');
        if (v > 65535) return -1;
        digits++; p++;
    }
    if (digits > 0) return v;
    /* 服务名表（覆盖 libcurl 常见项） */
    struct { const char *n; int p; } tbl[] = {
        {"http", 80}, {"https", 443}, {"ftp", 21}, {"ftp-data", 20},
        {"ssh", 22}, {"telnet", 23}, {"smtp", 25}, {"dns", 53}, {"domain", 53},
        {"tftp", 69}, {"ntp", 123}, {"pop3", 110}, {"imap", 143}, {"snmp", 161},
        {"http-alt", 8080}, {"https-alt", 8443},
    };
    for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++)
        if (strcmp(service, tbl[i].n) == 0) return tbl[i].p;
    return -1;
}

/* ===========================================================================
 * DNS-over-UDP 解析器（仅 A 记录）
 * 默认 DNS 服务器 10.0.2.3（QEMU user-net 内建转发器），可用 SUKI_DNS_SERVER 覆盖。
 * =========================================================================== */

#define DNS_SERVER_DEFAULT "10.0.2.3"
#define DNS_PORT 53

static int dns_resolve_a(const char *name, uint8_t out_ip[4]) {
    const char *server = getenv("SUKI_DNS_SERVER");
    if (!server) server = DNS_SERVER_DEFAULT;

    struct in_addr srv_ip;
    if (inet_pton(AF_INET, server, srv_ip.s_addr) != 1) return -1;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port   = (in_port_t)DNS_PORT; /* 主机序 */
    memcpy(srv.sin_addr.s_addr, srv_ip.s_addr, 4);

    static uint16_t dns_id = 0x1234;
    dns_id++;

    uint8_t query[256];
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > 250) { close(fd); return -1; }

    query[0] = (uint8_t)(dns_id >> 8);
    query[1] = (uint8_t)(dns_id & 0xff);
    query[2] = 0x01; query[3] = 0x00;        /* flags: RD */
    query[4] = 0x00; query[5] = 0x01;        /* QDCOUNT=1 */
    query[6] = 0x00; query[7] = 0x00;
    query[8] = 0x00; query[9] = 0x00;
    query[10] = 0x00; query[11] = 0x00;

    size_t off = 12;
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t lab = dot ? (size_t)(dot - p) : strlen(p);
        if (lab == 0 || lab > 63) { close(fd); return -1; }
        query[off++] = (uint8_t)lab;
        memcpy(query + off, p, lab);
        off += lab;
        p = dot ? dot + 1 : p + lab;
    }
    query[off++] = 0;                        /* 根 label */
    query[off++] = 0x00; query[off++] = 0x01; /* QTYPE A */
    query[off++] = 0x00; query[off++] = 0x01; /* QCLASS IN */
    size_t qlen = off;

    uint8_t resp[1024];
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    int got = -1;
    /* UDP 无链路层重传，查询或响应任一丢包都会导致仅发一次的解析永久挂起
     * （recv 无数据时会挂起 30s 等 net_server 回包，25 次即 750s）。
     * 故每轮重试都重发查询：服务器对相同查询幂等响应，可自愈丢包。 */
    for (int tries = 0; tries < 12; tries++) {
        long sw = sendto(fd, query, qlen, 0, (struct sockaddr *)&srv, sizeof(srv));
        if (sw < 0) { close(fd); return -1; }
        int pr = poll(&pfd, 1, 400);
        if (pr > 0) {
            got = recv(fd, resp, sizeof(resp), 0);
            if (got >= 12) break;
        }
    }
    close(fd);
    if (got < 12) return -1;
    if (resp[0] != query[0] || resp[1] != query[1]) return -1;

    /* 跳过问段（Question）。问段名可能是完整 label 序列，也可能是压缩指针
     * （0xC0 0x0C，指向 offset 12 处原始查询名）。两种情形下都须让 ro 精确停在
     * 「名结束之后、QTYPE 之前」，否则后续 Answer 段游标整体错位 1 字节会导致解析失败。
     * 关键：压缩指针结尾没有尾随的 0x00 根 label，故不能额外 ro++；只有完整序列才
     * 在退出循环时正好停在表示根 label 的 0x00 上，需 ro++ 跳过它。 */
    size_t ro = 12;
    while (ro < (size_t)got) {
        uint8_t l = resp[ro];
        if (l == 0)      { ro++; break; }    /* 根 label：名结束 */
        if (l & 0xC0)    { ro += 2; break; } /* 压缩指针：名结束 */
        ro += l + 1;                          /* 普通 label */
    }
    if (ro + 4 > (size_t)got) return -1;
    ro += 4;                                 /* QTYPE + QCLASS */

    uint16_t ancount = (uint16_t)((resp[6] << 8) | resp[7]);
    int found = 0;
    for (uint16_t i = 0; i < ancount && ro + 12 <= (size_t)got; i++) {
        if (resp[ro] & 0xC0) ro += 2;
        else {
            while (ro < (size_t)got && resp[ro] != 0) {
                uint8_t l = resp[ro];
                if (l & 0xC0) { ro += 2; break; }
                ro += l + 1;
            }
            ro++;
        }
        if (ro + 10 > (size_t)got) break;
        uint16_t type = (uint16_t)((resp[ro] << 8) | resp[ro + 1]);
        ro += 2;                              /* 跳过 TYPE */
        ro += 2;                              /* 跳过 CLASS */
        ro += 4;                              /* 跳过 TTL */
        uint16_t rdlen = (uint16_t)((resp[ro] << 8) | resp[ro + 1]);
        ro += 2;
        if (type == 0x0001 && rdlen == 4) {  /* A 记录 */
            out_ip[0] = resp[ro];
            out_ip[1] = resp[ro + 1];
            out_ip[2] = resp[ro + 2];
            out_ip[3] = resp[ro + 3];
            found = 1;
        }
        ro += rdlen;
    }
    return found ? 0 : -1;
}

/* ===========================================================================
 * 名称解析 API
 * =========================================================================== */

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
    if (res) *res = NULL;
    int passive = hints && (hints->ai_flags & AI_PASSIVE);
    if (!node && !passive) return EAI_NONAME;

    int family = hints ? hints->ai_family : AF_UNSPEC;
    int socktype = hints ? hints->ai_socktype : 0;
    int protocol = hints ? hints->ai_protocol : 0;
    if (family == AF_UNSPEC) family = AF_INET;   /* 默认仅 IPv4 */
    if (family != AF_INET) return EAI_FAMILY;

    int port = parse_service(service);
    if (port < 0) return EAI_SERVICE;

    struct addrinfo *ai = (struct addrinfo *)calloc(1, sizeof(struct addrinfo));
    if (!ai) return EAI_MEMORY;
    struct sockaddr_in *sin = (struct sockaddr_in *)calloc(1, sizeof(struct sockaddr_in));
    if (!sin) { free(ai); return EAI_MEMORY; }

    sin->sin_family = AF_INET;
    sin->sin_port   = (in_port_t)port;

    if (!node || passive) {
        memset(sin->sin_addr.s_addr, 0, 4);     /* 0.0.0.0 通配 */
    } else if (inet_pton(AF_INET, node, sin->sin_addr.s_addr) == 1) {
        /* 数字地址直通 */
    } else {
        uint8_t ip[4];
        if (dns_resolve_a(node, ip) != 0) {
            free(sin); free(ai);
            return EAI_NONAME;
        }
        memcpy(sin->sin_addr.s_addr, ip, 4);
    }

    ai->ai_family   = AF_INET;
    ai->ai_socktype = socktype ? socktype : SOCK_STREAM;
    ai->ai_protocol = protocol;
    ai->ai_addrlen  = sizeof(struct sockaddr_in);
    ai->ai_addr     = (struct sockaddr *)sin;
    ai->ai_next     = NULL;

    if (res) *res = ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *res) {
    while (res) {
        struct addrinfo *n = res->ai_next;
        if (res->ai_addr) free(res->ai_addr);
        free(res);
        res = n;
    }
}

const char *gai_strerror(int errcode) {
    switch (errcode) {
        case 0:           return "Success";
        case EAI_AGAIN:   return "Temporary failure in name resolution";
        case EAI_BADFLAGS:return "Invalid value for ai_flags";
        case EAI_FAIL:    return "Non-recoverable failure in name resolution";
        case EAI_FAMILY:  return "ai_family not supported";
        case EAI_MEMORY:  return "Memory allocation failure";
        case EAI_NONAME:  return "Name or service not known";
        case EAI_SERVICE: return "Service not supported for ai_socktype";
        case EAI_SOCKTYPE:return "ai_socktype not supported";
        case EAI_OVERFLOW:return "Argument buffer overflow";
        default:          return "Unknown error";
    }
}

struct hostent *gethostbyname(const char *name) {
    static struct hostent he;
    static char *alist[2];
    static char ipbuf[4];

    struct addrinfo *res = NULL;
    if (getaddrinfo(name, NULL, NULL, &res) != 0 || !res) return NULL;

    memset(&he, 0, sizeof(he));
    he.h_name     = (char *)name;
    he.h_addrtype = AF_INET;
    he.h_length   = 4;
    memcpy(ipbuf, ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr, 4);
    alist[0] = ipbuf;
    alist[1] = NULL;
    he.h_addr_list = alist;

    freeaddrinfo(res);
    return &he;
}

int getnameinfo(const struct sockaddr *sa, socklen_t salen,
                char *host, socklen_t hostlen, char *serv, socklen_t servlen,
                int flags) {
    (void)salen; (void)flags;
    const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
    if (host && hostlen) {
        if (inet_ntop(AF_INET, sin->sin_addr.s_addr, host, hostlen) == NULL)
            return EAI_OVERFLOW;
    }
    if (serv && servlen) {
        int n = snprintf(serv, servlen, "%u", (unsigned)sin->sin_port);
        if (n < 0 || n >= (int)servlen) return EAI_OVERFLOW;
    }
    return 0;
}
