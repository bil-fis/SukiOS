/*
 * user/lib/lwipopts.h
 * -----------------------------------------------------------------------------
 * lwIP 2.2.1 的移植配置（lwipopts.h）。
 *
 * SukiOS 适配要点：
 *   - NO_SYS=1：不使用 lwIP 的 OS 模拟层（无需线程/邮箱）。原因是 SukiNative
 *     的 SukiWait() 的 timeout_ms 尚未实现，而 lwIP 的顺序/socket API 依赖
 *     带超时的 mbox 等待来实现 TCP 定时器；改用 raw API 后，定时器由网络服务
 *     周期性调用 sys_check_timeouts() 驱动，阻塞语义放到 IPC 层（客户端等待
 *     服务应答），这正是微内核网络服务的典型结构，也更稳健。
 *   - 内存：MEM_LIBC_MALLOC=1 + MEMP_MEM_MALLOC=1，全部走用户态 malloc/free，
 *     避免在本就紧张的用户地址空间里再占用大块 lwIP 静态堆/池数组。
 *   - 只启用 IPv4 + ARP + ICMP + UDP + TCP + DHCP + DNS；关闭 IPv6/IGMP/
 *     AUTOIP/STATS/DEBUG/SOCKET/NETCONN 以缩小体积与依赖面。
 */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/* ---- 运行模型 ---- */
#define NO_SYS                      1
#define LWIP_TIMERS                 1
#define LWIP_TIMERS_CUSTOM          0
#define LWIP_ALTCP                  0   /* 关闭 TLS 透传层（不需要 altcp.c） */
#define SYS_LIGHTWEIGHT_PROT        0   /* NO_SYS=1 单线程：保护为 no-op */

/* ---- 字符串比较：用 lwIP 内置实现，避免依赖 libc 的 strncasecmp/strcasecmp ---- */
#define LWIP_STRICMP                0
#define LWIP_STRNICMP               0

/* ---- 内存 ---- */
#define MEM_LIBC_MALLOC             1
#define MEMP_MEM_MALLOC             1
#define MEM_ALIGNMENT               4

/* ---- 协议族 ---- */
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_IGMP                   0
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_DHCP                   1
#define LWIP_AUTOIP                 0
#define LWIP_DNS                    1
#define LWIP_BROADCAST_PING         1
#define LWIP_RAW                    1

/* ---- 关闭不需要的上层与统计，减小体积/依赖 ---- */
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define LWIP_STATS                  0
#define LWIP_DEBUG                  0
#define LWIP_NETIF_API              0
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    0
#define LWIP_DHCP_DOES_ACD_CHECK    0
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_GET_NTP_SRV       0
#define LWIP_CHECKSUM_ON_COPY       0
#define LWIP_NETIF_LOOPBACK         0

/* ---- 缓冲与连接规模 ---- */
#define PBUF_POOL_SIZE              16
#define PBUF_POOL_BUFSIZE           1536
#define MEMP_NUM_PBUF               16
#define MEMP_NUM_RAW_PCB            4
#define MEMP_NUM_UDP_PCB            8
#define MEMP_NUM_TCP_PCB            8
#define MEMP_NUM_TCP_PCB_LISTEN     4
#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_REASSDATA          4
#define MEMP_NUM_ARP_QUEUE          8
#define MEMP_NUM_SYS_TIMEOUT        8
#define MEMP_NUM_DNS_REQ            2

/* ---- TCP 参数 ---- */
#define TCP_MSS                     1460
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            (4 * TCP_SND_BUF / TCP_MSS)
#define TCP_MAXRTX                  12
#define TCP_SYNMAXRTX               6
#define TCP_QUEUE_OOSEQ             1
#define TCP_OVERSIZE                0

/* ---- 其它 ---- */
#define ETH_PAD_SIZE                0
#define LWIP_TCPIP_CORE_LOCKING     0
#define LWIP_NETIF_TX_SINGLE_PBUF   0   /* 低层 output 已用 pbuf_copy_partial 拼接整帧 */
#define SO_REUSE                    0

#endif /* LWIPOPTS_H */
