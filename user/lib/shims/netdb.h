/*
 * user/lib/shims/netdb.h
 * -----------------------------------------------------------------------------
 * SukiOS 用户态标准 <netdb.h>：DNS 解析相关结构与服务名解析。
 * 实现见 user/lib/net.c（getaddrinfo 含数字地址直通 + DNS-over-UDP 解析器）。
 */
#ifndef _SUKI_SHIM_NETDB_H
#define _SUKI_SHIM_NETDB_H

#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>
#include <netinet/in.h>

struct hostent {
    char  *h_name;       /* 官方名 */
    char **h_aliases;    /* 别名数组（NULL 结尾） */
    int    h_addrtype;   /* AF_INET */
    int    h_length;     /* 地址长度（4） */
    char **h_addr_list;  /* 地址数组（网络序/主机序八位组） */
#define h_addr h_addr_list[0]
};

struct addrinfo {
    int             ai_flags;
    int             ai_family;
    int             ai_socktype;
    int             ai_protocol;
    socklen_t       ai_addrlen;
    struct sockaddr *ai_addr;
    char            *ai_canonname;
    struct addrinfo *ai_next;
};

/* ai_flags */
#define AI_PASSIVE      0x1
#define AI_CANONNAME    0x2
#define AI_NUMERICHOST  0x4
#define AI_NUMERICSERV  0x8
#define AI_V4MAPPED     0x10
#define AI_ALL          0x20
#define AI_ADDRCONFIG   0x40

/* EAI_* 错误码（getaddrinfo 返回正值错误码，与 glibc 一致） */
#define EAI_AGAIN    2
#define EAI_BADFLAGS 3
#define EAI_FAIL     4
#define EAI_FAMILY   6
#define EAI_MEMORY   8
#define EAI_NONAME   9
#define EAI_SERVICE  11
#define EAI_SOCKTYPE 12
#define EAI_OVERFLOW 14

/* 实现见 user/lib/net.c */
int  getaddrinfo(const char *node, const char *service,
                 const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
const char *gai_strerror(int errcode);
struct hostent *gethostbyname(const char *name);
int  getnameinfo(const struct sockaddr *sa, socklen_t salen,
                 char *host, socklen_t hostlen, char *serv, socklen_t servlen,
                 int flags);

#endif /* _SUKI_SHIM_NETDB_H */
