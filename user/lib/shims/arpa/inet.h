/*
 * user/lib/shims/arpa/inet.h
 * -----------------------------------------------------------------------------
 * SukiOS 用户态标准 <arpa/inet.h>：地址转换函数声明。
 * 实现见 user/lib/net.c。
 */
#ifndef _SUKI_SHIM_ARPA_INET_H
#define _SUKI_SHIM_ARPA_INET_H

#include <stdint.h>
#include <stddef.h>
#include <netinet/in.h>

/* 文本 <-> 二进制地址转换（AF_INET/AF_INET6）。成功返回 1，失败返回 0。
 * dst 至少 4 字节（IPv4）/16 字节（IPv6）。 */
int        inet_pton(int af, const char *src, void *dst);
/* 二进制 -> 文本，写入 size 字节缓冲区，返回 dst 或失败返回 NULL。 */
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
/* 点分十进制 -> 32 位主机序 IPv4 地址；非法返回 INADDR_NONE。 */
in_addr_t  inet_addr(const char *cp);

#endif /* _SUKI_SHIM_ARPA_INET_H */
