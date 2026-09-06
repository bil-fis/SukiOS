/*
 * user/net_server.c
 * -----------------------------------------------------------------------------
 * SukiOS Ring3 网络服务（lwIP 协议栈 + e1000 帧收发，经 NET_PORT Mach IPC）。
 *
 * 架构（与 FS_SERVER/disk-srv 同构，符合混合内核红线）：
 *   - lwIP 跑在用户态（NO_SYS=1：raw API，定时器由本服务轮询 sys_check_timeouts
 *     驱动；阻塞语义放在 IPC 层——客户端等 net_server 应答，绝不在 lwIP 内部阻塞
 *     内核）。这是微内核网络服务的典型结构，且不依赖 suki_wait 的 timeout（尚未实现）。
 *   - 原始以太网帧经 NET_PORT 收发：net_server 是 NET_PORT 的【客户端】，发
 *     GET_MAC/SEND/RECV 请求，并以 NET_REPLY_PORT 收应答；
 *     Ring0 的 e1000 驱动（kernel/drivers/e1000.c::net_srv_task）才是 NET_PORT 的
 *     【服务端】，只做帧收发、绝不解析 IP/TCP/UDP。
 *
 * 本文件当前实现：e1000 链路 + lwIP + DHCP（QEMU user-net 自带网关/DHCP
 * 10.0.2.2），启动后自动获取 IP 并打印，证明「e1000 驱动 → NET_PORT → lwIP
 * 协议栈」全链路打通。后续里程碑（socket 服务 / POSIX / SukiNative NET 对象）
 * 将在 NS_PORT 之上叠加。
 *
 * 生产约束（铁律）：所有 IPC 收发都用本任务本地静态缓冲，绝不把外部指针当
 * 内存目标；畸形消息直接丢弃；无 stub/TODO。
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>          /* 用户态 shim：CLOCK_MONOTONIC / struct timespec /
                             clock_gettime / nanosleep */

#include "lib/suki.h"
#include <ipc/net_proto.h>

#include <lwip/init.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/etharp.h>
#include <lwip/dhcp.h>
#include <lwip/timeouts.h>
#include <lwip/sys.h>
#include <lwip/ip_addr.h>
#include <lwip/ip4_addr.h>
#include <netif/ethernet.h>

/* ---- 与 NET_PORT 的请求/应答缓冲（本任务私有，杜绝外部指针写）---- */
static uint8_t g_req[sizeof(mach_msg_header_t) + sizeof(net_send_req_t)
                     + NET_MAX_FRAME + 16];
static uint8_t g_resp[sizeof(mach_msg_header_t) + sizeof(net_recv_resp_t)
                      + NET_MAX_FRAME + 16];

/* ===================== 时间 / 随机（lwIP 端口必须提供） ===================== */

/* sys_now()：返回自启动起的毫秒数（NO_SYS=1 下 lwIP 定时器依赖它） */
u32_t sys_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u32_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

/* LWIP_RAND() 的实现（cc.h 宏展开为 lwip_port_rand()）：xorshift32。
 * 种子由 sys_now 派生，确保每次启动序列不同（DHCP 事务 ID 等）。 */
static uint32_t g_rand_state = 0x9e3779b9u;
unsigned int lwip_port_rand(void)
{
    uint32_t x = g_rand_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rand_state = x;
    return x;
}

/* ===================== NET_PORT 客户端封装 ===================== */

/* 发送一条 NET_PORT 请求（msgh_local_port=NET_REPLY_PORT），并在 NET_REPLY_PORT
 * 上收应答；返回应答总字节数（含头），0 表示 IPC 失败。 */
static uint32_t net_transact(uint32_t id, const void *payload, uint32_t plen)
{
    mach_msg_header_t *h = (mach_msg_header_t *)g_req;
    h->msgh_bits        = 0;
    h->msgh_size        = (uint32_t)(sizeof(mach_msg_header_t) + plen);
    h->msgh_remote_port = NET_PORT;
    h->msgh_local_port  = NET_REPLY_PORT;
    h->msgh_id          = id;
    h->msgh_reserved    = 0;
    if (plen && payload)
        memcpy(g_req + sizeof(mach_msg_header_t), payload, plen);

    mach_msg_send(g_req, h->msgh_size);

    uint32_t lim = (uint32_t)sizeof(g_resp);
    if (mach_msg_recv(g_resp, lim, NET_REPLY_PORT) != MACH_MSG_SUCCESS)
        return 0;
    return ((mach_msg_header_t *)g_resp)->msgh_size;
}

static bool net_get_mac(uint8_t mac[6])
{
    uint32_t n = net_transact(NET_MSG_GET_MAC, NULL, 0);
    if (n < sizeof(mach_msg_header_t) + sizeof(net_mac_resp_t))
        return false;
    net_mac_resp_t *mr = (net_mac_resp_t *)(g_resp + sizeof(mach_msg_header_t));
    if (mr->status != 0)
        return false;
    memcpy(mac, mr->mac, 6);
    return true;
}

static bool net_send_frame(const uint8_t *data, uint32_t len)
{
    if (len == 0 || len > NET_MAX_FRAME)
        return false;
    uint8_t buf[sizeof(net_send_req_t) + NET_MAX_FRAME];
    net_send_req_t *sr = (net_send_req_t *)buf;
    sr->length = len;
    memcpy(buf + sizeof(net_send_req_t), data, len);

    uint32_t n = net_transact(NET_MSG_SEND, buf,
                              (uint32_t)sizeof(net_send_req_t) + len);
    if (n < sizeof(mach_msg_header_t) + sizeof(net_send_resp_t))
        return false;
    net_send_resp_t *sp = (net_send_resp_t *)(g_resp + sizeof(mach_msg_header_t));
    return sp->status == 0;
}

/* 收一帧：发出 NET_MSG_RECV 请求，内核最多轮询 20ms 后应答（无帧则 length=0）。
 * 返回帧字节数（>0），0 表示本次无帧，-1 表示 IPC 失败。 */
static int net_recv_frame(uint8_t *out, uint32_t max)
{
    uint32_t n = net_transact(NET_MSG_RECV, NULL, 0);
    if (n < sizeof(mach_msg_header_t) + sizeof(net_recv_resp_t))
        return -1;
    net_recv_resp_t *rr = (net_recv_resp_t *)(g_resp + sizeof(mach_msg_header_t));
    if (rr->length == 0)
        return 0;
    uint32_t cpy = rr->length < max ? rr->length : max;   /* 防御越界 */
    memcpy(out, g_resp + sizeof(mach_msg_header_t) + sizeof(net_recv_resp_t), cpy);
    return (int)cpy;
}

/* ===================== lwIP 网络接口 ===================== */

static struct netif g_netif;

/* 链路层输出：把 pbuf 链拷成连续帧，经 NET_PORT 发出 */
static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
    (void)netif;
    if (p->tot_len > NET_MAX_FRAME)
        return ERR_MEM;
    uint8_t frame[NET_MAX_FRAME];
    uint16_t got = pbuf_copy_partial(p, frame, p->tot_len, 0);
    if (got != p->tot_len)
        return ERR_MEM;
    if (!net_send_frame(frame, p->tot_len))
        return ERR_IF;
    return ERR_OK;
}

static err_t netif_init_cb(struct netif *netif)
{
    netif->linkoutput = low_level_output;
    netif->output      = etharp_output;
    netif->mtu         = 1500;
    netif->hwaddr_len  = 6;
    netif->flags       = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP
                         | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

/* ===================== 主循环 ===================== */

static void print_mac(const uint8_t m[6])
{
    char buf[32];
    uint32_t i = 0;
    for (int b = 0; b < 6; b++) {
        static const char hexd[] = "0123456789abcdef";
        uint8_t v = m[b];
        buf[i++] = hexd[(v >> 4) & 0xF];
        buf[i++] = hexd[v & 0xF];
        if (b < 5) buf[i++] = ':';
    }
    buf[i] = '\0';
    u_print(buf);
}

int main(void)
{
    u_print("[net] net_server starting (lwIP 2.2.1, NO_SYS raw API)\n");
    sys_port_claim(NET_REPLY_PORT);

    uint8_t mac[6];
    if (!net_get_mac(mac)) {
        u_print("[net] NET_PORT unavailable: no e1000 / no link\n");
        return 1;
    }
    u_print("[net] MAC=");
    print_mac(mac);
    u_print("\n");

    /* 播种随机数 */
    g_rand_state = sys_now() ^ 0x9e3779b9u;

    lwip_init();

    ip4_addr_t ipa, mask, gw;
    memset(&ipa,  0, sizeof(ipa));
    memset(&mask, 0, sizeof(mask));
    memset(&gw,   0, sizeof(gw));

    netif_add(&g_netif, &ipa, &mask, &gw, NULL, netif_init_cb, ethernet_input);
    netif_set_default(&g_netif);
    memcpy(g_netif.hwaddr, mac, 6);
    g_netif.hwaddr_len = 6;
    netif_set_up(&g_netif);

    err_t e = dhcp_start(&g_netif);
    if (e != ERR_OK) {
        u_print("[net] dhcp_start failed (err ");
        char d[12]; d[0] = '0' + (char)(e & 0xF); d[1] = '\0';
        u_print(d);
        u_print(")\n");
    } else {
        u_print("[net] DHCP discover sent; awaiting offer...\n");
    }

    bool ip_printed = false;
    uint8_t rx[NET_MAX_FRAME];

    for (;;) {
        /* 驱动 DHCP/TCP 等定时器（NO_SYS=1 下必须周期性调用） */
        sys_check_timeouts();

        int got = net_recv_frame(rx, sizeof(rx));
        if (got > 0) {
            struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)got, PBUF_POOL);
            if (p) {
                if (pbuf_take(p, rx, (u16_t)got) == ERR_OK) {
                    if (g_netif.input(p, &g_netif) != ERR_OK)
                        pbuf_free(p);          /* input 失败则此处释放 */
                } else {
                    pbuf_free(p);
                }
            }
        }

        if (!ip_printed && dhcp_supplied_address(&g_netif)) {
            ip_printed = true;
            u_print("[net] DHCP BOUND: ip=");
            u_print((char *)ip4addr_ntoa(ip_2_ip4(&g_netif.ip_addr)));
            u_print(" gw=");
            u_print((char *)ip4addr_ntoa(ip_2_ip4(&g_netif.gw)));
            u_print(" mask=");
            u_print((char *)ip4addr_ntoa(ip_2_ip4(&g_netif.netmask)));
            u_print("\n");
        }

        struct timespec ts = { 0, 5 * 1000 * 1000 };   /* 5ms：节流 + 让出 CPU */
        nanosleep(&ts, NULL);
    }
    return 0;
}
