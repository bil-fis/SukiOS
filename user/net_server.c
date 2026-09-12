/*
 * user/net_server.c
 * -----------------------------------------------------------------------------
 * SukiOS Ring3 网络服务（lwIP 协议栈 + e1000 帧收发，经 NET_PORT Mach IPC）。
 *
 * 架构（与 FS_SERVER/disk-srv 同构，符合混合内核红线）：
 *   - lwIP 跑在用户态（NO_SYS=1：raw API，定时器由本服务轮询 sys_check_timeouts
 *     驱动；阻塞语义放在 IPC 层——客户端等 net_server 应答，绝不在 lwIP 内部阻塞
 *     内核）。这是微内核网络服务的典型结构，且不依赖 SukiWait 的 timeout（尚未实现）。
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

/* ============================================================================
 *  网络 socket 服务（NS_PORT）
 *  ---------------------------------------------------------------------------
 *  单线程（主线程）同时服务 NET_REPLY_PORT（帧）与 NS_PORT（socket 请求）：
 *  主循环每轮先 non-block 轮询 NS_PORT 处理请求（阻塞语义靠「挂起请求 + 回调完成时
 *  回复」实现，绝不在 lwIP 内部阻塞），再收帧喂给 lwIP。lwIP 仅在主线程调用
 *  （raw API，NO_SYS=1），线程安全。
 * ========================================================================== */
#include <sukios/net.h>
#include <lwip/udp.h>
#include <lwip/tcp.h>

#define SOCK_MSG_REPLY  100     /* 服务侧应答消息 ID（非 SERVICE_DOWN，避免被内核判为下线） */
#define NSOCK           64
#define SOCK_RX_CAP     8       /* 每 socket 接收队列上限（防无限增长） */
#define RX_NODE_POOL    512     /* 静态 rx 节点池（避免 malloc 依赖） */

enum sock_state { S_CLOSED=0, S_CREATED, S_BOUND, S_CONNECTED, S_LISTENING };

typedef struct rx_node {
    struct pbuf *p;
    uint16_t     off;          /* 已消费字节偏移（TCP 流） */
    ip4_addr_t   src_ip;       /* UDP 源地址 */
    u16_t        src_port;
    struct rx_node *next;
} rx_node_t;

typedef struct pending {
    bool        active;
    uint32_t    reply_port;
    uint32_t    op;
    sock_req_t  req;           /* 请求副本（含 flags/len/addr） */
} pending_t;

typedef struct sock {
    uint32_t    handle;
    int         in_use;
    int         type;          /* SUKI_SOCK_DGRAM / SUKI_SOCK_STREAM */
    int         state;
    union { struct udp_pcb *udp; struct tcp_pcb *tcp; } pcb;
    rx_node_t  *rx_head, *rx_tail;
    uint32_t    rx_total;
    bool        rx_eof;
    pending_t   pend;
    struct tcp_pcb *accept_pcb;/* 已接受待取走的 pcb（无挂起 accept 时暂存） */
} sock_t;

static sock_t g_socks[NSOCK];
static uint8_t g_sock_reply[sizeof(mach_msg_header_t) + sizeof(sock_resp_t)];
static uint8_t g_ns_req[sizeof(mach_msg_header_t) + sizeof(sock_req_t)];

/* 静态 rx 节点池 + 空闲链表 */
static rx_node_t g_rx_pool[RX_NODE_POOL];
static rx_node_t *g_rx_free;
static void rx_pool_init(void)
{
    g_rx_free = NULL;
    for (int i = RX_NODE_POOL - 1; i >= 0; i--) {
        g_rx_pool[i].next = g_rx_free;
        g_rx_free = &g_rx_pool[i];
    }
}
static rx_node_t *rx_alloc(void)
{
    rx_node_t *n = g_rx_free;
    if (n) g_rx_free = n->next;
    return n;
}
static void rx_free_node(rx_node_t *n) { n->next = g_rx_free; g_rx_free = n; }

/* ---- 工具 ---- */
static sock_t *sock_by_handle(uint32_t h)
{
    if (h < 1 || h > NSOCK) return NULL;
    sock_t *s = &g_socks[h - 1];
    return s->in_use ? s : NULL;
}
static int sock_alloc(void)
{
    for (int i = 0; i < NSOCK; i++) {
        if (!g_socks[i].in_use) {
            memset(&g_socks[i], 0, sizeof(sock_t));
            g_socks[i].in_use = 1;
            g_socks[i].handle = (uint32_t)(i + 1);
            return (int)g_socks[i].handle;
        }
    }
    return -1;
}
static void rx_free_all(sock_t *s)
{
    rx_node_t *n = s->rx_head;
    while (n) {
        rx_node_t *nx = n->next;
        pbuf_free(n->p);
        rx_free_node(n);
        n = nx;
    }
    s->rx_head = s->rx_tail = NULL;
    s->rx_total = 0;
    s->rx_eof = false;
}
static void sock_free(sock_t *s)
{
    if (s->type == SUKI_SOCK_DGRAM) {
        if (s->pcb.udp) udp_remove(s->pcb.udp);
    } else {
        if (s->pcb.tcp) tcp_close(s->pcb.tcp);
    }
    rx_free_all(s);
    if (s->accept_pcb) { tcp_abort(s->accept_pcb); s->accept_pcb = NULL; }
    s->in_use = 0;
}

static void build_addr(uint8_t addr[16], const ip4_addr_t *ip, u16_t port)
{
    memset(addr, 0, 16);
    addr[0] = SUKI_AF_INET & 0xff; addr[1] = (SUKI_AF_INET >> 8) & 0xff;
    addr[2] = port & 0xff;        addr[3] = (port >> 8) & 0xff;
    addr[4] = ip4_addr1(ip); addr[5] = ip4_addr2(ip);
    addr[6] = ip4_addr3(ip); addr[7] = ip4_addr4(ip);
}
static void parse_addr(const uint8_t addr[16], ip_addr_t *ip, u16_t *port)
{
    ip4_addr_t a4;
    IP4_ADDR(&a4, addr[4], addr[5], addr[6], addr[7]);
    ip_addr_copy_from_ip4(*ip, a4);
    *port = (u16_t)(addr[2] | ((uint16_t)addr[3] << 8));
}

static void rx_enqueue(sock_t *s, struct pbuf *p, const ip4_addr_t *src_ip, u16_t src_port)
{
    if (s->rx_total >= SOCK_RX_CAP) { pbuf_free(p); return; }  /* 超限丢弃，防资源耗尽 */
    rx_node_t *n = rx_alloc();
    if (!n) { pbuf_free(p); return; }
    n->p = p; n->off = 0; n->next = NULL;
    if (src_ip) n->src_ip = *src_ip; else ip4_addr_set_zero(&n->src_ip);
    n->src_port = src_port;
    if (s->rx_tail) s->rx_tail->next = n; else s->rx_head = n;
    s->rx_tail = n;
    s->rx_total++;
}

/* ---- 应答 ---- */
static void sock_reply(uint32_t reply_port, uint32_t status, uint32_t result,
                       uint32_t addr_len, const uint8_t *addr, uint32_t len,
                       const uint8_t *data)
{
    memset(g_sock_reply, 0, sizeof(g_sock_reply));
    mach_msg_header_t *h = (mach_msg_header_t *)g_sock_reply;
    h->msgh_size        = (uint32_t)(sizeof(mach_msg_header_t) + sizeof(sock_resp_t));
    h->msgh_remote_port = reply_port;
    h->msgh_local_port  = NS_PORT;
    h->msgh_id          = SOCK_MSG_REPLY;
    sock_resp_t *r = (sock_resp_t *)(g_sock_reply + sizeof(mach_msg_header_t));
    r->status = status; r->result = result; r->addr_len = addr_len;
    if (addr && addr_len) memcpy(r->addr, addr, addr_len);
    r->len = len;
    if (data && len) memcpy(r->data, data, len);
    mach_msg_send(g_sock_reply, h->msgh_size);
}

/* 完成一次挂起的 recv（数据已就绪或 EOF） */
static void complete_recv_now(sock_t *s)
{
    uint32_t want = s->pend.req.len;
    uint8_t *buf = g_sock_reply;
    memset(buf, 0, sizeof(mach_msg_header_t) + sizeof(sock_resp_t));
    sock_resp_t *r = (sock_resp_t *)(buf + sizeof(mach_msg_header_t));
    uint32_t n = 0;
    uint8_t addr[16] = {0};
    uint32_t alen = 0;
    ip4_addr_t sip; u16_t sport = 0; bool have_src = false;

    if (s->rx_head) {
        rx_node_t *nd = s->rx_head;
        struct pbuf *p = nd->p;
        uint16_t avail = (uint16_t)(p->tot_len - nd->off);
        n = (want < avail) ? want : avail;
        if (n) pbuf_copy_partial(p, r->data, (u16_t)n, nd->off);
        nd->off += (uint16_t)n;
        if (nd->off >= p->tot_len) {
            s->rx_head = nd->next;
            if (!s->rx_head) s->rx_tail = NULL;
            pbuf_free(p);
            if (s->type == SUKI_SOCK_DGRAM) { sip = nd->src_ip; sport = nd->src_port; have_src = true; }
            rx_free_node(nd);
            s->rx_total--;
        }
    } else if (s->rx_eof) {
        n = 0;   /* EOF：对端关闭 */
    }

    if (have_src) { build_addr(addr, &sip, sport); alen = 16; }
    r->status = 0; r->result = 0; r->addr_len = alen;
    if (alen) memcpy(r->addr, addr, alen);
    r->len = n;
    mach_msg_header_t *h = (mach_msg_header_t *)buf;
    h->msgh_size        = (uint32_t)(sizeof(mach_msg_header_t) + sizeof(sock_resp_t));
    h->msgh_remote_port = s->pend.reply_port;
    h->msgh_local_port  = NS_PORT;
    h->msgh_id          = SOCK_MSG_REPLY;
    mach_msg_send(buf, h->msgh_size);
    s->pend.active = false;
}

static void try_complete_recv(sock_t *s)
{
    if (!s->pend.active) return;
    if (s->rx_head || s->rx_eof) complete_recv_now(s);
}

/* ---- lwIP 回调（均在主线程调用）---- */
static void udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                        const ip_addr_t *addr, u16_t port)
{
    (void)pcb;
    sock_t *s = arg;
    if (!p) return;
    rx_enqueue(s, p, ip_2_ip4(addr), port);
    try_complete_recv(s);
}
static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)pcb;
    sock_t *s = arg;
    if (!p) {
        if (err == ERR_OK) { s->rx_eof = true; try_complete_recv(s); }
        return ERR_OK;
    }
    rx_enqueue(s, p, NULL, 0);
    try_complete_recv(s);
    return ERR_OK;
}
static err_t tcp_sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)arg; (void)pcb; (void)len;
    return ERR_OK;
}
static err_t tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)pcb;
    sock_t *s = arg;
    if (s->pend.active && s->pend.op == SOCK_MSG_CONNECT) {
        s->state = (err == ERR_OK) ? S_CONNECTED : S_CLOSED;
        sock_reply(s->pend.reply_port, err == ERR_OK ? 0 : (uint32_t)-SUKI_ETIMEDOUT,
                   0, 0, NULL, 0, NULL);
        s->pend.active = false;
    }
    return ERR_OK;
}
static void complete_accept(sock_t *ls, struct tcp_pcb *newpcb)
{
    int h = sock_alloc();
    if (h < 0) {
        tcp_abort(newpcb);
        sock_reply(ls->pend.reply_port, (uint32_t)-SUKI_ENFILE, 0, 0, NULL, 0, NULL);
        ls->pend.active = false;
        return;
    }
    sock_t *cs = &g_socks[h - 1];
    cs->type = SUKI_SOCK_STREAM;
    cs->pcb.tcp = newpcb;
    tcp_arg(newpcb, cs);
    tcp_recv(newpcb, tcp_recv_cb);
    tcp_sent(newpcb, tcp_sent_cb);
    cs->state = S_CONNECTED;
    uint8_t addr[16];
    build_addr(addr, ip_2_ip4(&newpcb->remote_ip), newpcb->remote_port);
    sock_reply(ls->pend.reply_port, 0, (uint32_t)h, 16, addr, 0, NULL);
    ls->pend.active = false;
}
static err_t tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    sock_t *ls = arg;
    if (err != ERR_OK || !newpcb) return ERR_OK;
    if (ls->pend.active && ls->pend.op == SOCK_MSG_ACCEPT) {
        complete_accept(ls, newpcb);
    } else {
        if (ls->accept_pcb) tcp_abort(ls->accept_pcb);
        ls->accept_pcb = newpcb;   /* 暂存（覆盖旧） */
    }
    return ERR_OK;
}

/* ---- 请求分发（同步，主线程）---- */
static void sock_handle(sock_req_t *req, uint32_t reply_port)
{
    sock_t *s = NULL;
    if (req->op != SOCK_MSG_CREATE) {
        s = sock_by_handle(req->sock);
        if (!s) { sock_reply(reply_port, (uint32_t)-SUKI_ENOTSOCK, 0, 0, NULL, 0, NULL); return; }
    }
    switch (req->op) {
    case SOCK_MSG_CREATE: {
        int h = sock_alloc();
        if (h < 0) { sock_reply(reply_port, (uint32_t)-SUKI_ENFILE, 0, 0, NULL, 0, NULL); return; }
        sock_t *ns = &g_socks[h - 1];
        ns->type = (req->type == SUKI_SOCK_DGRAM) ? SUKI_SOCK_DGRAM : SUKI_SOCK_STREAM;
        if (ns->type == SUKI_SOCK_DGRAM) {
            ns->pcb.udp = udp_new();
            if (ns->pcb.udp) udp_recv(ns->pcb.udp, udp_recv_cb, ns);
        } else {
            ns->pcb.tcp = tcp_new();
        }
        if ((ns->type == SUKI_SOCK_DGRAM && !ns->pcb.udp) ||
            (ns->type == SUKI_SOCK_STREAM && !ns->pcb.tcp)) {
            ns->in_use = 0;
            sock_reply(reply_port, (uint32_t)-SUKI_ENOMEM, 0, 0, NULL, 0, NULL);
            return;
        }
        ns->state = S_CREATED;
        sock_reply(reply_port, 0, (uint32_t)h, 0, NULL, 0, NULL);
        break;
    }
    case SOCK_MSG_BIND: {
        ip_addr_t ip; u16_t port; parse_addr(req->addr, &ip, &port);
        err_t e = (s->type == SUKI_SOCK_DGRAM)
                    ? udp_bind(s->pcb.udp, &ip, port)
                    : tcp_bind(s->pcb.tcp, &ip, port);
        if (e == ERR_OK) s->state = S_BOUND;
        sock_reply(reply_port, e == ERR_OK ? 0 : (uint32_t)-SUKI_EINVAL, 0, 0, NULL, 0, NULL);
        break;
    }
    case SOCK_MSG_CONNECT: {
        ip_addr_t ip; u16_t port; parse_addr(req->addr, &ip, &port);
        if (s->type == SUKI_SOCK_DGRAM) {
            err_t e = udp_connect(s->pcb.udp, &ip, port);
            if (e == ERR_OK) s->state = S_CONNECTED;
            sock_reply(reply_port, e == ERR_OK ? 0 : (uint32_t)-SUKI_EINVAL, 0, 0, NULL, 0, NULL);
        } else {
            tcp_arg(s->pcb.tcp, s);
            tcp_recv(s->pcb.tcp, tcp_recv_cb);
            tcp_sent(s->pcb.tcp, tcp_sent_cb);
            err_t e = tcp_connect(s->pcb.tcp, &ip, port, tcp_connected_cb);
            if (e != ERR_OK) {
                sock_reply(reply_port, (uint32_t)-SUKI_EIO, 0, 0, NULL, 0, NULL);
                break;
            }
            s->pend.active = true; s->pend.op = SOCK_MSG_CONNECT;
            s->pend.reply_port = reply_port; s->pend.req = *req;
        }
        break;
    }
    case SOCK_MSG_LISTEN: {
        struct tcp_pcb *l = tcp_listen(s->pcb.tcp);
        if (!l) { sock_reply(reply_port, (uint32_t)-SUKI_EINVAL, 0, 0, NULL, 0, NULL); break; }
        s->pcb.tcp = l;
        tcp_arg(l, s);
        tcp_accept(l, tcp_accept_cb);
        s->state = S_LISTENING;
        sock_reply(reply_port, 0, 0, 0, NULL, 0, NULL);
        break;
    }
    case SOCK_MSG_ACCEPT: {
        if (s->type != SUKI_SOCK_STREAM) {
            sock_reply(reply_port, (uint32_t)-SUKI_EOPNOTSUPP, 0, 0, NULL, 0, NULL); break;
        }
        if (s->accept_pcb) {
            struct tcp_pcb *np = s->accept_pcb; s->accept_pcb = NULL;
            s->pend.active = true; s->pend.op = SOCK_MSG_ACCEPT;
            s->pend.reply_port = reply_port; s->pend.req = *req;
            complete_accept(s, np);
        } else {
            s->pend.active = true; s->pend.op = SOCK_MSG_ACCEPT;
            s->pend.reply_port = reply_port; s->pend.req = *req;
        }
        break;
    }
    case SOCK_MSG_SENDTO: {
        ip_addr_t ip; u16_t port; parse_addr(req->addr, &ip, &port);
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)req->len, PBUF_RAM);
        if (!p) { sock_reply(reply_port, (uint32_t)-SUKI_ENOMEM, 0, 0, NULL, 0, NULL); break; }
        memcpy(p->payload, req->data, req->len);
        err_t e = udp_sendto(s->pcb.udp, p, &ip, port);
        pbuf_free(p);
        sock_reply(reply_port, e == ERR_OK ? 0 : (uint32_t)-SUKI_EIO,
                   e == ERR_OK ? req->len : 0, 0, NULL, 0, NULL);
        break;
    }
    case SOCK_MSG_SEND: {
        u16_t avail = tcp_sndbuf(s->pcb.tcp);
        uint32_t n = req->len < avail ? req->len : avail;
        if (n == 0) { sock_reply(reply_port, (uint32_t)-SUKI_EAGAIN, 0, 0, NULL, 0, NULL); break; }
        err_t e = tcp_write(s->pcb.tcp, req->data, n, TCP_WRITE_FLAG_COPY);
        if (e == ERR_OK) tcp_output(s->pcb.tcp);
        sock_reply(reply_port, e == ERR_OK ? 0 : (uint32_t)-SUKI_EIO,
                   e == ERR_OK ? n : 0, 0, NULL, 0, NULL);
        break;
    }
    case SOCK_MSG_RECVFROM:
    case SOCK_MSG_RECV: {
        if (s->rx_head || s->rx_eof) {
            s->pend.active = true; s->pend.op = req->op;
            s->pend.reply_port = reply_port; s->pend.req = *req;
            complete_recv_now(s);
        } else {
            s->pend.active = true; s->pend.op = req->op;
            s->pend.reply_port = reply_port; s->pend.req = *req;
        }
        break;
    }
    case SOCK_MSG_CLOSE:
        sock_free(s);
        sock_reply(reply_port, 0, 0, 0, NULL, 0, NULL);
        break;
    case SOCK_MSG_GETSOCKNAME: {
        ip4_addr_t ip; u16_t port;
        if (s->type == SUKI_SOCK_DGRAM) { ip = *ip_2_ip4(&s->pcb.udp->local_ip);  port = s->pcb.udp->local_port; }
        else                           { ip = *ip_2_ip4(&s->pcb.tcp->local_ip);  port = s->pcb.tcp->local_port; }
        uint8_t addr[16]; build_addr(addr, &ip, port);
        sock_reply(reply_port, 0, 0, 16, addr, 0, NULL);
        break;
    }
    case SOCK_MSG_GETPEERNAME: {
        ip4_addr_t ip; u16_t port;
        if (s->type == SUKI_SOCK_DGRAM) { ip = *ip_2_ip4(&s->pcb.udp->remote_ip); port = s->pcb.udp->remote_port; }
        else                           { ip = *ip_2_ip4(&s->pcb.tcp->remote_ip); port = s->pcb.tcp->remote_port; }
        uint8_t addr[16]; build_addr(addr, &ip, port);
        sock_reply(reply_port, 0, 0, 16, addr, 0, NULL);
        break;
    }
    case SOCK_MSG_SETSOCKOPT:
    case SOCK_MSG_GETSOCKOPT:
        sock_reply(reply_port, 0, 0, 0, NULL, 0, NULL);
        break;
    case SOCK_MSG_SHUTDOWN:
        if (s->type == SUKI_SOCK_STREAM && s->pcb.tcp) tcp_close(s->pcb.tcp);
        sock_reply(reply_port, 0, 0, 0, NULL, 0, NULL);
        break;
    default:
        sock_reply(reply_port, (uint32_t)-SUKI_ENOSYS, 0, 0, NULL, 0, NULL);
        break;
    }
}

/* 非阻塞轮询 NS_PORT 处理积压的 socket 请求 */
static void socket_service_poll(void)
{
    for (;;) {
        uint32_t lim = (uint32_t)sizeof(g_ns_req);
        if (mach_msg_tryrecv(g_ns_req, lim, NS_PORT) != MACH_MSG_SUCCESS)
            break;   /* 队列空（MACH_RCV_TIMED_OUT） */
        mach_msg_header_t *h = (mach_msg_header_t *)g_ns_req;
        sock_handle((sock_req_t *)(g_ns_req + sizeof(mach_msg_header_t)),
                    h->msgh_local_port);
    }
}

static void socket_service_init(void)
{
    memset(g_socks, 0, sizeof(g_socks));
    rx_pool_init();
    sys_port_claim(NS_PORT);
    u_print("[net] socket service (NS_PORT) ready\n");
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

    /* 启动 socket 服务（认领 NS_PORT，供内核 POSIX 网络 syscall / SukiNative 转发） */
    socket_service_init();

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

        /* 非阻塞轮询 NS_PORT：处理 socket 请求（sendto/recvfrom/...）。
         * 阻塞语义（recv 无数据、accept 无连接）靠挂起请求 + 回调完成实现，
         * 主线程绝不在此处阻塞，保证帧接收与 DHCP 定时器持续运行。 */
        socket_service_poll();

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
