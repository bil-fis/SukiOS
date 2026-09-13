/*
 * kernel/net/socket.c
 * -----------------------------------------------------------------------------
 * 内核网络子系统：POSIX 网络 syscall（150..164）分发 + fd 集成。
 *
 * 定位（见 include/kernel/net/socket.h）：内核不解析任何网络协议，仅把 socket 操作
 * 翻译成 NS_PORT 的 IPC 消息，由用户态 net_server（lwIP 2.2.1 raw API）执行。与
 * fd.c 转发 FS_PORT 完全同构——这是混合内核红线（协议栈用户态化）的必然结构。
 *
 * 红线（与 fd.c 一致）：
 *   - 用户指针一律经 copy_from_user/copy_to_user；
 *   - IPC 往返（会 schedule 让出）在 fd 锁外执行；
 *   - 绝不信任外部长度：应答的 len/result 先与真实字节数比对再拷贝。
 */
#include <kernel/net/socket.h>
#include <kernel/syscall.h>
#include <kernel/task.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/console.h>
#include <kernel/clock.h>       /* clock_monotonic_ns：为 net RPC 提供有界等待时钟 */
#include <ipc/port.h>
#include <kernel/percpu.h>
#include <mm/kmalloc.h>

/* per-CPU 请求/应答缓冲（持锁外阻塞，必须 per-CPU，见不变量 B） */
static uint8_t g_net_req[MAX_CPUS][NET_RPC_MAX];
static uint8_t g_net_resp[MAX_CPUS][NET_RPC_MAX];

static inline uint8_t *net_req_buf(void)  { return g_net_req[cpu_index()]; }
static inline uint8_t *net_resp_buf(void) { return g_net_resp[cpu_index()]; }

/* net RPC 有界等待上限：SukiWait 的 timeout_ms 尚未实现，若用 ipc_recv_kernel(...,
 * true) 无限阻塞，socket 的 recv/connect 在等不到 net_server 回送（对端无数据 /
 * 连接实际未通）时会永久挂起，连带冻结调用它的用户任务（如 exec 中的 curl）与
 * shell。故此处改以非阻塞轮询 + task_yield 实现有界等待，超时上抛 -SUKI_ETIMEDOUT。 */
#ifndef NET_RPC_TIMEOUT_NS
#define NET_RPC_TIMEOUT_NS (30ULL * 1000000000ULL)   /* 30 秒 */
#endif

/*
 * 向 NS_PORT 发一条 socket 请求并同步等待应答。返回 0 成功（应答在 net_resp_buf），
 * 否则负 errno。等待有界（见 NET_RPC_TIMEOUT_NS），避免无限阻塞（详见上方说明）。
 */
static int net_rpc(uint32_t op, const void *payload, uint32_t req_size)
{
    (void)op;
    (void)payload;
    task_t *self = sched_current();
    uint32_t rp = port_allocate(self);
    if (!rp) {
        return -SUKI_ENFILE;
    }
    mach_msg_header_t *h = (mach_msg_header_t *)net_req_buf();
    h->msgh_local_port  = rp;
    h->msgh_remote_port = NS_PORT;

    if (ipc_send_kernel(NS_PORT, net_req_buf(), req_size) != MACH_MSG_SUCCESS) {
        port_free(rp);
        return -SUKI_EIO;
    }

    /* 有界等待：非阻塞轮询 + 让出 CPU，直到收到应答或超时。 */
    uint64_t deadline = clock_monotonic_ns() + NET_RPC_TIMEOUT_NS;
    int rc = -SUKI_ETIMEDOUT;
    for (;;) {
        uint32_t got = 0;
        if (ipc_recv_kernel(rp, net_resp_buf(), (uint32_t)NET_RPC_MAX, &got, false)
                == MACH_MSG_SUCCESS) {
            rc = 0;
            break;
        }
        if ((int64_t)(deadline - clock_monotonic_ns()) <= 0)
            break;
        task_yield();
    }
    port_free(rp);

    if (rc != 0)
        return rc;

    /* 服务下线占位应答（net_server 已死，理论上不会发生，做防御） */
    mach_msg_header_t *rh = (mach_msg_header_t *)net_resp_buf();
    if (rh->msgh_id == SOCK_MSG_SERVICE_DOWN) {
        return -SUKI_EIO;
    }
    return 0;
}

/* 准备一条请求：清零缓冲、填头与公共字段，返回 sock_req_t 指针供调用方续填 */
static sock_req_t *net_req_prep(uint32_t op, uint32_t sock)
{
    uint8_t *b = net_req_buf();
    memset(b, 0, sizeof(mach_msg_header_t) + sizeof(sock_req_t));
    mach_msg_header_t *h = (mach_msg_header_t *)b;
    h->msgh_size = (uint32_t)(sizeof(mach_msg_header_t) + sizeof(sock_req_t));
    h->msgh_id   = op;
    sock_req_t *r = (sock_req_t *)(b + sizeof(mach_msg_header_t));
    r->op   = op;
    r->sock = sock;
    return r;
}

static inline sock_resp_t *net_resp(void)
{
    return (sock_resp_t *)(net_resp_buf() + sizeof(mach_msg_header_t));
}

/* 把用户 sockaddr（suki_sockaddr_in 布局，host 序）拷入 16 字节 addr 缓冲 */
static int copy_addr_from_user(const void *uaddr, uint32_t addrlen, uint8_t addr[16])
{
    if (!uaddr) {
        return 0;
    }
    uint32_t n = addrlen > 16 ? 16 : addrlen;
    if (copy_from_user(addr, uaddr, n) != n) {
        return -SUKI_EFAULT;
    }
    return 0;
}

/* 把 16 字节 addr 拷回用户，并回写 addrlen（若指针非空） */
static void copy_addr_to_user(const uint8_t addr[16], uint32_t alen,
                              void *uaddr, uint32_t *uaddrlen)
{
    if (uaddr && alen) {
        uint32_t n = alen > 16 ? 16 : alen;
        copy_to_user(uaddr, addr, n);
    }
    if (uaddrlen) {
        copy_to_user(uaddrlen, &alen, sizeof(alen));
    }
}

/* ---- fd 集成：socket fd -> sock handle ---- */
static fd_entry_t *net_fd_entry(struct task *t, int fd)
{
    fd_entry_t *e = fd_get(t, fd);
    if (!e || e->type != FD_TYPE_SOCKET) {
        return NULL;
    }
    return e;
}

/*
 * net_poll：查询 socket fd 的就绪掩码（POSIX POLL* 位）。
 * 经 SOCK_MSG_POLL 问 net_server（lwIP 状态），net_server 在 resp.result 回传掩码。
 * fd 非 socket / 查询失败时返回 0。
 */
uint32_t net_poll(int fd, uint32_t want)
{
    task_t *t = sched_current();
    fd_entry_t *e = net_fd_entry(t, fd);
    if (!e) {
        return 0;
    }
    sock_req_t *r = net_req_prep(SOCK_MSG_POLL, (uint32_t)e->backend);
    r->flags = want;
    if (net_rpc(SOCK_MSG_POLL, NULL,
                (uint32_t)(sizeof(mach_msg_header_t) + sizeof(sock_req_t))) != 0) {
        return 0;
    }
    sock_resp_t *rp = net_resp();
    if ((int32_t)rp->status < 0) {
        return 0;
    }
    return rp->result;
}

/* ===================== 各操作实现 ===================== */

static int do_socket(int domain, int type, int proto, int *out_fd)
{
    sock_req_t *r = net_req_prep(SOCK_MSG_CREATE, 0);
    r->domain = (uint32_t)domain;
    r->type   = (uint32_t)type;
    r->proto  = (uint32_t)proto;
    if (net_rpc(SOCK_MSG_CREATE, NULL,
                (uint32_t)(sizeof(mach_msg_header_t) + sizeof(sock_req_t))) != 0) {
        return -SUKI_EIO;
    }
    sock_resp_t *rp = net_resp();
    if (rp->status != 0) {
        return (int)rp->status;
    }
    int fd = fd_socket(sched_current(), (int)rp->result);
    if (fd < 0) {
        return -SUKI_ENFILE;
    }
    *out_fd = fd;
    return 0;
}

static int do_bind(struct task *t, int fd, const void *addr, uint32_t addrlen)
{
    fd_entry_t *e = net_fd_entry(t, fd);
    if (!e) {
        return -SUKI_ENOTSOCK;
    }
    uint8_t a[16];
    if (copy_addr_from_user(addr, addrlen, a) != 0) {
        return -SUKI_EFAULT;
    }
    sock_req_t *r = net_req_prep(SOCK_MSG_BIND, (uint32_t)e->backend);
    r->addr_len = addrlen > 16 ? 16 : addrlen;
    memcpy(r->addr, a, r->addr_len);
    if (net_rpc(SOCK_MSG_BIND, NULL,
                (uint32_t)(sizeof(mach_msg_header_t) + sizeof(sock_req_t))) != 0) {
        return -SUKI_EIO;
    }
    return (int)net_resp()->status;
}

static int do_connect(struct task *t, int fd, const void *addr, uint32_t addrlen)
{
    fd_entry_t *e = net_fd_entry(t, fd);
    if (!e) {
        return -SUKI_ENOTSOCK;
    }
    uint8_t a[16];
    if (copy_addr_from_user(addr, addrlen, a) != 0) {
        return -SUKI_EFAULT;
    }
    sock_req_t *r = net_req_prep(SOCK_MSG_CONNECT, (uint32_t)e->backend);
    r->addr_len = addrlen > 16 ? 16 : addrlen;
    memcpy(r->addr, a, r->addr_len);
    if (net_rpc(SOCK_MSG_CONNECT, NULL,
                (uint32_t)(sizeof(mach_msg_header_t) + sizeof(sock_req_t))) != 0) {
        return -SUKI_EIO;
    }
    return (int)net_resp()->status;
}

static int do_listen(struct task *t, int fd, int backlog)
{
    (void)backlog;
    fd_entry_t *e = net_fd_entry(t, fd);
    if (!e) {
        return -SUKI_ENOTSOCK;
    }
    net_req_prep(SOCK_MSG_LISTEN, (uint32_t)e->backend);
    if (net_rpc(SOCK_MSG_LISTEN, NULL,
                (uint32_t)(sizeof(mach_msg_header_t) + sizeof(sock_req_t))) != 0) {
        return -SUKI_EIO;
    }
    return (int)net_resp()->status;
}

static int do_accept(struct task *t, int fd, void *addr, uint32_t *addrlen)
{
    fd_entry_t *e = net_fd_entry(t, fd);
    if (!e) {
        return -SUKI_ENOTSOCK;
    }
    net_req_prep(SOCK_MSG_ACCEPT, (uint32_t)e->backend);
    if (net_rpc(SOCK_MSG_ACCEPT, NULL,
                (uint32_t)(sizeof(mach_msg_header_t) + sizeof(sock_req_t))) != 0) {
        return -SUKI_EIO;
    }
    sock_resp_t *rp = net_resp();
    if (rp->status != 0) {
        return (int)rp->status;
    }
    int newfd = fd_socket(t, (int)rp->result);
    if (newfd < 0) {
        return -SUKI_ENFILE;
    }
    copy_addr_to_user(rp->addr, rp->addr_len, addr, addrlen);
    return newfd;
}

static long do_sendto(struct task *t, int fd, const void *buf, size_t len,
                      uint32_t flags, const void *addr, uint32_t addrlen)
{
    (void)flags;
    fd_entry_t *e = net_fd_entry(t, fd);
    if (!e) {
        return -SUKI_ENOTSOCK;
    }
    if (len > SOCK_MAX_DATA) {
        return -SUKI_EMSGSIZE;
    }
    sock_req_t *r = net_req_prep(SOCK_MSG_SENDTO, (uint32_t)e->backend);
    if (len && copy_from_user(r->data, buf, len) != len) {
        return -SUKI_EFAULT;
    }
    r->len = (uint32_t)len;
    if (addr) {
        uint8_t a[16];
        if (copy_addr_from_user(addr, addrlen, a) != 0) {
            return -SUKI_EFAULT;
        }
        r->addr_len = addrlen > 16 ? 16 : addrlen;
        memcpy(r->addr, a, r->addr_len);
    }
    if (net_rpc(SOCK_MSG_SENDTO, NULL,
                (uint32_t)(sizeof(mach_msg_header_t) + sizeof(sock_req_t))) != 0) {
        return -SUKI_EIO;
    }
    sock_resp_t *rp = net_resp();
    if (rp->status != 0) {
        return (long)rp->status;
    }
    return (long)rp->result;   /* 实际发送字节数 */
}

static long do_send(struct task *t, int fd, const void *buf, size_t len, uint32_t flags)
{
    (void)flags;
    fd_entry_t *e = net_fd_entry(t, fd);
    if (!e) {
        return -SUKI_ENOTSOCK;
    }
    if (len > SOCK_MAX_DATA) {
        return -SUKI_EMSGSIZE;
    }
    sock_req_t *r = net_req_prep(SOCK_MSG_SEND, (uint32_t)e->backend);
    if (len && copy_from_user(r->data, buf, len) != len) {
        return -SUKI_EFAULT;
    }
    r->len = (uint32_t)len;
    if (net_rpc(SOCK_MSG_SEND, NULL,
                (uint32_t)(sizeof(mach_msg_header_t) + sizeof(sock_req_t))) != 0) {
        return -SUKI_EIO;
    }
    sock_resp_t *rp = net_resp();
    if (rp->status != 0) {
        return (long)rp->status;
    }
    return (long)rp->result;
}

static long do_recvfrom(struct task *t, int fd, void *buf, size_t len,
                        uint32_t flags, void *addr, uint32_t *addrlen)
{
    (void)flags;
    fd_entry_t *e = net_fd_entry(t, fd);
    if (!e) {
        return -SUKI_ENOTSOCK;
    }
    sock_req_t *r = net_req_prep(SOCK_MSG_RECVFROM, (uint32_t)e->backend);
    r->len = (uint32_t)len;
    if (net_rpc(SOCK_MSG_RECVFROM, NULL,
                (uint32_t)(sizeof(mach_msg_header_t) + sizeof(sock_req_t))) != 0) {
        return -SUKI_EIO;
    }
    sock_resp_t *rp = net_resp();
    if (rp->status != 0) {
        return (long)rp->status;
    }
    uint32_t n = rp->len;
    if (n > len) {
        n = (uint32_t)len;     /* 防御：截断到用户缓冲容量 */
    }
    if (n && copy_to_user(buf, rp->data, n) != n) {
        return -SUKI_EFAULT;
    }
    copy_addr_to_user(rp->addr, rp->addr_len, addr, addrlen);
    return (long)n;
}

static long do_recv(struct task *t, int fd, void *buf, size_t len, uint32_t flags)
{
    return do_recvfrom(t, fd, buf, len, flags, NULL, NULL);
}

static int do_shutdown(struct task *t, int fd, int how)
{
    (void)how;
    fd_entry_t *e = net_fd_entry(t, fd);
    if (!e) {
        return -SUKI_ENOTSOCK;
    }
    net_req_prep(SOCK_MSG_SHUTDOWN, (uint32_t)e->backend);
    if (net_rpc(SOCK_MSG_SHUTDOWN, NULL,
                (uint32_t)(sizeof(mach_msg_header_t) + sizeof(sock_req_t))) != 0) {
        return -SUKI_EIO;
    }
    return (int)net_resp()->status;
}

static int do_setsockopt(struct task *t, int fd, int level, int optname,
                         const void *optval, uint32_t optlen)
{
    fd_entry_t *e = net_fd_entry(t, fd);
    if (!e) {
        return -SUKI_ENOTSOCK;
    }
    sock_req_t *r = net_req_prep(SOCK_MSG_SETSOCKOPT, (uint32_t)e->backend);
    r->domain = (uint32_t)level;     /* 复用字段：level */
    r->type   = (uint32_t)optname;   /* 复用字段：optname */
    r->addr_len = optlen > 16 ? 16 : optlen;
    if (optlen && optval && copy_from_user(r->addr, optval, r->addr_len) != r->addr_len) {
        return -SUKI_EFAULT;
    }
    if (net_rpc(SOCK_MSG_SETSOCKOPT, NULL,
                (uint32_t)(sizeof(mach_msg_header_t) + sizeof(sock_req_t))) != 0) {
        return -SUKI_EIO;
    }
    return (int)net_resp()->status;
}

static int do_getsockopt(struct task *t, int fd, int level, int optname,
                         void *optval, uint32_t *optlen)
{
    fd_entry_t *e = net_fd_entry(t, fd);
    if (!e) {
        return -SUKI_ENOTSOCK;
    }
    sock_req_t *r = net_req_prep(SOCK_MSG_GETSOCKOPT, (uint32_t)e->backend);
    r->domain = (uint32_t)level;
    r->type   = (uint32_t)optname;
    uint32_t inlen = 0;
    if (optlen && copy_from_user(&inlen, optlen, sizeof(inlen)) != sizeof(inlen)) {
        return -SUKI_EFAULT;
    }
    r->addr_len = inlen > 16 ? 16 : inlen;
    if (net_rpc(SOCK_MSG_GETSOCKOPT, NULL,
                (uint32_t)(sizeof(mach_msg_header_t) + sizeof(sock_req_t))) != 0) {
        return -SUKI_EIO;
    }
    sock_resp_t *rp = net_resp();
    if (rp->status != 0) {
        return (int)rp->status;
    }
    uint32_t n = rp->len > inlen ? inlen : rp->len;
    if (n && optval && copy_to_user(optval, rp->data, n) != n) {
        return -SUKI_EFAULT;
    }
    if (optlen) {
        copy_to_user(optlen, &rp->len, sizeof(rp->len));
    }
    return 0;
}

static int do_getpeername(struct task *t, int fd, void *addr, uint32_t *addrlen)
{
    fd_entry_t *e = net_fd_entry(t, fd);
    if (!e) {
        return -SUKI_ENOTSOCK;
    }
    net_req_prep(SOCK_MSG_GETPEERNAME, (uint32_t)e->backend);
    if (net_rpc(SOCK_MSG_GETPEERNAME, NULL,
                (uint32_t)(sizeof(mach_msg_header_t) + sizeof(sock_req_t))) != 0) {
        return -SUKI_EIO;
    }
    sock_resp_t *rp = net_resp();
    if (rp->status != 0) {
        return (int)rp->status;
    }
    copy_addr_to_user(rp->addr, rp->addr_len, addr, addrlen);
    return 0;
}

static int do_getsockname(struct task *t, int fd, void *addr, uint32_t *addrlen)
{
    fd_entry_t *e = net_fd_entry(t, fd);
    if (!e) {
        return -SUKI_ENOTSOCK;
    }
    net_req_prep(SOCK_MSG_GETSOCKNAME, (uint32_t)e->backend);
    if (net_rpc(SOCK_MSG_GETSOCKNAME, NULL,
                (uint32_t)(sizeof(mach_msg_header_t) + sizeof(sock_req_t))) != 0) {
        return -SUKI_EIO;
    }
    sock_resp_t *rp = net_resp();
    if (rp->status != 0) {
        return (int)rp->status;
    }
    copy_addr_to_user(rp->addr, rp->addr_len, addr, addrlen);
    return 0;
}

static int do_socketpair(struct task *t, int domain, int type, int proto, int *fds)
{
    (void)t; (void)domain; (void)type; (void)proto; (void)fds;
    return -SUKI_EOPNOTSUPP;   /* AF_UNIX 未实现 */
}

/* ===================== fd.c 集成 ===================== */

void net_close_backend(fd_entry_t *e)
{
    if (!e || e->backend < 0) {
        return;
    }
    net_req_prep(SOCK_MSG_CLOSE, (uint32_t)e->backend);
    (void)net_rpc(SOCK_MSG_CLOSE, NULL,
                  (uint32_t)(sizeof(mach_msg_header_t) + sizeof(sock_req_t)));
    e->backend = -1;
}

suki_ssize_t net_read(struct task *t, int fd, void *ubuf, size_t count)
{
    long n = do_recv(t, fd, ubuf, count, 0);
    return (suki_ssize_t)n;
}

suki_ssize_t net_write(struct task *t, int fd, const void *ubuf, size_t count)
{
    long n = do_send(t, fd, ubuf, count, 0);
    return (suki_ssize_t)n;
}

/* ===================== 分发表 ===================== */

uint64_t sys_net_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct task *t = sched_current();
    switch (num) {
    case SYS_SOCKET: {
        int fd = -1;
        int rc = do_socket((int)a1, (int)a2, (int)a3, &fd);
        return rc < 0 ? (uint64_t)rc : (uint64_t)fd;
    }
    case SYS_BIND:        return (uint64_t)do_bind(t, (int)a1, (void *)a2, (uint32_t)a3);
    case SYS_CONNECT:     return (uint64_t)do_connect(t, (int)a1, (void *)a2, (uint32_t)a3);
    case SYS_LISTEN:      return (uint64_t)do_listen(t, (int)a1, (int)a2);
    case SYS_ACCEPT:      return (uint64_t)do_accept(t, (int)a1, (void *)a2, (uint32_t *)a3);
    case SYS_SENDTO:      return (uint64_t)do_sendto(t, (int)a1, (void *)a2, (size_t)a3,
                                                       (uint32_t)a4, (void *)a5, (uint32_t)a6);
    case SYS_RECVFROM:    return (uint64_t)do_recvfrom(t, (int)a1, (void *)a2, (size_t)a3,
                                                         (uint32_t)a4, (void *)a5, (uint32_t *)a6);
    case SYS_SEND:        return (uint64_t)do_send(t, (int)a1, (void *)a2, (size_t)a3, (uint32_t)a4);
    case SYS_RECV:        return (uint64_t)do_recv(t, (int)a1, (void *)a2, (size_t)a3, (uint32_t)a4);
    case SYS_SHUTDOWN:    return (uint64_t)do_shutdown(t, (int)a1, (int)a2);
    case SYS_SETSOCKOPT:  return (uint64_t)do_setsockopt(t, (int)a1, (int)a2, (int)a3,
                                                          (void *)a4, (uint32_t)a5);
    case SYS_GETSOCKOPT:  return (uint64_t)do_getsockopt(t, (int)a1, (int)a2, (int)a3,
                                                          (void *)a4, (uint32_t *)a5);
    case SYS_GETPEERNAME: return (uint64_t)do_getpeername(t, (int)a1, (void *)a2, (uint32_t *)a3);
    case SYS_GETSOCKNAME: return (uint64_t)do_getsockname(t, (int)a1, (void *)a2, (uint32_t *)a3);
    case SYS_SOCKETPAIR:  return (uint64_t)do_socketpair(t, (int)a1, (int)a2, (int)a3, (int *)a4);
    default:              return (uint64_t)-SUKI_ENOSYS;
    }
}
