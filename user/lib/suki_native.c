/*
 * user/lib/suki_native.c
 * -----------------------------------------------------------------------------
 * SukiNative 原生对象 API 的用户态封装（libsuki）。对应内核
 * kernel/syscall/sys_suki.c 的 130..149 号系统调用。
 *
 * 与 POSIX 平行：新服务可直接调用这些原生接口（一切皆对象，返回 suki_handle_t
 * 句柄，用 suki_status_t 状态码 0/负 errno）。POSIX 兼容层（20..129）不受影响。
 *
 * __suki_syscallN 的声明来自 <sukios/posix.h>（static inline 内联汇编，ABI：
 * rax=号，rdi/rsi/rdx/r10/r8/r9=参数）。
 */
#include "suki.h"
#include <sukios/posix.h>
#include <sukios/net.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

suki_status_t SukiEventCreate(bool init_signaled, bool manual_reset, suki_handle_t *out)
{
    int64_t h = __suki_syscall3(SYS_SUKI_EVENT_CREATE,
                                (uint64_t)init_signaled, (uint64_t)manual_reset, 0);
    if (h < 0)
        return (suki_status_t)h;
    *out = (suki_handle_t)h;
    return 0;
}

suki_status_t SukiEventSet(suki_handle_t h)
{
    return (suki_status_t)__suki_syscall1(SYS_SUKI_EVENT_SET, (uint64_t)h);
}

suki_status_t SukiEventReset(suki_handle_t h)
{
    return (suki_status_t)__suki_syscall1(SYS_SUKI_EVENT_RESET, (uint64_t)h);
}

suki_status_t SukiMutexCreate(suki_handle_t *out)
{
    int64_t h = __suki_syscall1(SYS_SUKI_MUTEX_CREATE, 0);
    if (h < 0)
        return (suki_status_t)h;
    *out = (suki_handle_t)h;
    return 0;
}

suki_status_t SukiMutexLock(suki_handle_t h)
{
    return (suki_status_t)__suki_syscall1(SYS_SUKI_MUTEX_LOCK, (uint64_t)h);
}

suki_status_t SukiMutexUnlock(suki_handle_t h)
{
    return (suki_status_t)__suki_syscall1(SYS_SUKI_MUTEX_UNLOCK, (uint64_t)h);
}

suki_status_t SukiSemCreate(uint32_t initial, uint32_t max, suki_handle_t *out)
{
    int64_t h = __suki_syscall3(SYS_SUKI_SEM_CREATE,
                                (uint64_t)initial, (uint64_t)max, 0);
    if (h < 0)
        return (suki_status_t)h;
    *out = (suki_handle_t)h;
    return 0;
}

suki_status_t SukiSemAcquire(suki_handle_t h)
{
    return (suki_status_t)__suki_syscall1(SYS_SUKI_SEM_ACQUIRE, (uint64_t)h);
}

suki_status_t SukiSemRelease(suki_handle_t h)
{
    return (suki_status_t)__suki_syscall1(SYS_SUKI_SEM_RELEASE, (uint64_t)h);
}

suki_status_t SukiWait(const suki_handle_t *handles, size_t count, uint32_t flags,
                       uint64_t timeout_ms, size_t *out_index)
{
    (void)timeout_ms;   /* 暂未支持超时：阻塞至被唤醒 */
    size_t idx = 0;
    int64_t r = __suki_syscall6(SYS_SUKI_WAIT,
                                (uint64_t)handles, (uint64_t)count, (uint64_t)flags,
                                0, (uint64_t)&idx, 0);
    if (r == 0)
        *out_index = idx;
    return (suki_status_t)r;
}

suki_status_t SukiObjDestroy(suki_handle_t h)
{
    return (suki_status_t)__suki_syscall1(SYS_SUKI_OBJ_DESTROY, (uint64_t)h);
}

suki_status_t SukiObjDuplicate(suki_handle_t h, uint32_t rights, suki_handle_t *out)
{
    int64_t nh = __suki_syscall2(SYS_SUKI_OBJ_DUPLICATE, (uint64_t)h, (uint64_t)rights);
    if (nh < 0)
        return (suki_status_t)nh;
    *out = (suki_handle_t)nh;
    return 0;
}

suki_status_t SukiObjQuery(suki_handle_t h, suki_objinfo_t *info)
{
    return (suki_status_t)__suki_syscall2(SYS_SUKI_OBJ_QUERY,
                                          (uint64_t)h, (uint64_t)info);
}

suki_status_t SukiObjCreate(uint32_t type, uint64_t a2, uint64_t a3, suki_handle_t *out)
{
    int64_t h = __suki_syscall3(SYS_SUKI_OBJ_CREATE, (uint64_t)type, a2, a3);
    if (h < 0)
        return (suki_status_t)h;
    *out = (suki_handle_t)h;
    return 0;
}

/* ---- SukiNative 文件对象（144..147）---- */
suki_status_t SukiFileOpen(const char *path, uint32_t access, suki_handle_t *out)
{
    int64_t h = __suki_syscall4(SYS_SUKI_FILE_OPEN,
                                (uint64_t)path, (uint64_t)access, 0, (uint64_t)out);
    if (h < 0)
        return (suki_status_t)h;
    *out = (suki_handle_t)h;
    return 0;
}

suki_status_t SukiFileRead(suki_handle_t h, void *buf, size_t count, size_t *out_nread)
{
    return (suki_status_t)__suki_syscall4(SYS_SUKI_FILE_READ,
                                          (uint64_t)h, (uint64_t)buf, (uint64_t)count,
                                          (uint64_t)out_nread);
}

suki_status_t SukiFileWrite(suki_handle_t h, const void *buf, size_t count, size_t *out_nwritten)
{
    return (suki_status_t)__suki_syscall4(SYS_SUKI_FILE_WRITE,
                                          (uint64_t)h, (uint64_t)buf, (uint64_t)count,
                                          (uint64_t)out_nwritten);
}

suki_status_t SukiFileClose(suki_handle_t h)
{
    return (suki_status_t)__suki_syscall1(SYS_SUKI_FILE_CLOSE, (uint64_t)h);
}

/* ---- SukiNative 进程对象（148）---- */
suki_status_t SukiProcCreate(const char *path, int argc, const char **argv, suki_handle_t *out)
{
    int64_t h = __suki_syscall4(SYS_SUKI_PROC_CREATE,
                                (uint64_t)path, (uint64_t)argv, (uint64_t)argc,
                                (uint64_t)out);
    if (h < 0)
        return (suki_status_t)h;
    *out = (suki_handle_t)h;
    return 0;
}

/* ---- SukiNative 内存对象（149）---- */
suki_status_t SukiMemAlloc(uint64_t size, suki_handle_t *out)
{
    int64_t h = __suki_syscall2(SYS_SUKI_MEM_ALLOC, (uint64_t)size, (uint64_t)out);
    if (h < 0)
        return (suki_status_t)h;
    *out = (suki_handle_t)h;
    return 0;
}

/* ============================================================================
 *  SukiNative 网络 socket 对象（POSIX 150..164 的「原生」平行接口）
 *  一切皆对象、直接 IPC 到 NS_PORT，不经过内核 POSIX 层。客户端用 sys_port_alloc
 *  取一个临时应答端口（每进程缓存一个，单线程同步调用安全）。
 * ========================================================================== */
static uint8_t  g_sreq[sizeof(mach_msg_header_t) + sizeof(sock_req_t)];
static uint8_t  g_sresp[sizeof(mach_msg_header_t) + sizeof(sock_resp_t)];
static uint32_t g_sock_rp;

static void sock_req_header(uint32_t op, uint32_t sock)
{
    if (!g_sock_rp) {
        g_sock_rp = (uint32_t)suki_syscall5(SYS_PORT_ALLOC, 0, 0, 0, 0, 0);
    }
    memset(g_sreq, 0, sizeof(g_sreq));
    mach_msg_header_t *h = (mach_msg_header_t *)g_sreq;
    h->msgh_size        = (uint32_t)sizeof(g_sreq);
    h->msgh_id          = op;
    h->msgh_remote_port = NS_PORT;
    h->msgh_local_port  = g_sock_rp;
    sock_req_t *r = (sock_req_t *)(g_sreq + sizeof(mach_msg_header_t));
    r->op   = op;
    r->sock = sock;
}

static suki_status_t sock_ipc(void)
{
    if (mach_msg_send(g_sreq, (uint32_t)sizeof(g_sreq)) != MACH_MSG_SUCCESS)
        return (suki_status_t)-1;
    if (mach_msg_recv(g_sresp, (uint32_t)sizeof(g_sresp), g_sock_rp) != MACH_MSG_SUCCESS)
        return (suki_status_t)-1;
    return (suki_status_t)((sock_resp_t *)(g_sresp + sizeof(mach_msg_header_t)))->status;
}

static sock_resp_t *sock_resp(void)
{
    return (sock_resp_t *)(g_sresp + sizeof(mach_msg_header_t));
}

suki_status_t SukiSocketCreate(int domain, int type, int proto, suki_socket_t *out)
{
    sock_req_header(SOCK_MSG_CREATE, 0);
    sock_req_t *r = (sock_req_t *)(g_sreq + sizeof(mach_msg_header_t));
    r->domain = (uint32_t)domain; r->type = (uint32_t)type; r->proto = (uint32_t)proto;
    suki_status_t st = sock_ipc();
    if (st != 0) return st;
    *out = sock_resp()->result;
    return 0;
}

suki_status_t SukiSocketBind(suki_socket_t s, const void *addr, uint32_t addrlen)
{
    sock_req_header(SOCK_MSG_BIND, s);
    sock_req_t *r = (sock_req_t *)(g_sreq + sizeof(mach_msg_header_t));
    if (addr && addrlen) { uint32_t n = addrlen > 16 ? 16 : addrlen; memcpy(r->addr, addr, n); r->addr_len = n; }
    return sock_ipc();
}

suki_status_t SukiSocketConnect(suki_socket_t s, const void *addr, uint32_t addrlen)
{
    sock_req_header(SOCK_MSG_CONNECT, s);
    sock_req_t *r = (sock_req_t *)(g_sreq + sizeof(mach_msg_header_t));
    if (addr && addrlen) { uint32_t n = addrlen > 16 ? 16 : addrlen; memcpy(r->addr, addr, n); r->addr_len = n; }
    return sock_ipc();
}

suki_status_t SukiSocketListen(suki_socket_t s, int backlog)
{
    (void)backlog;
    sock_req_header(SOCK_MSG_LISTEN, s);
    return sock_ipc();
}

suki_status_t SukiSocketAccept(suki_socket_t s, void *addr, uint32_t *addrlen, suki_socket_t *out)
{
    sock_req_header(SOCK_MSG_ACCEPT, s);
    suki_status_t st = sock_ipc();
    if (st != 0) return st;
    sock_resp_t *rp = sock_resp();
    if (addr && addrlen && rp->addr_len) { uint32_t n = rp->addr_len > 16 ? 16 : rp->addr_len; memcpy(addr, rp->addr, n); *addrlen = rp->addr_len; }
    *out = rp->result;
    return 0;
}

suki_status_t SukiSocketSend(suki_socket_t s, const void *buf, uint32_t len,
                               uint32_t flags, uint32_t *out_nwritten)
{
    (void)flags;
    sock_req_header(SOCK_MSG_SEND, s);
    sock_req_t *r = (sock_req_t *)(g_sreq + sizeof(mach_msg_header_t));
    uint32_t n = len > SOCK_MAX_DATA ? SOCK_MAX_DATA : len;
    if (n && buf) memcpy(r->data, buf, n);
    r->len = n;
    suki_status_t st = sock_ipc();
    if (st != 0) return st;
    if (out_nwritten) *out_nwritten = sock_resp()->result;
    return 0;
}

suki_status_t SukiSocketRecv(suki_socket_t s, void *buf, uint32_t len,
                               uint32_t flags, uint32_t *out_nread)
{
    (void)flags;
    sock_req_header(SOCK_MSG_RECV, s);
    sock_req_t *r = (sock_req_t *)(g_sreq + sizeof(mach_msg_header_t));
    r->len = len;
    suki_status_t st = sock_ipc();
    if (st != 0) return st;
    sock_resp_t *rp = sock_resp();
    uint32_t n = rp->len > len ? len : rp->len;
    if (n && buf) memcpy(buf, rp->data, n);
    if (out_nread) *out_nread = n;
    return 0;
}

suki_status_t SukiSocketSendTo(suki_socket_t s, const void *buf, uint32_t len,
                                 uint32_t flags, const void *addr, uint32_t addrlen,
                                 uint32_t *out_nwritten)
{
    (void)flags;
    sock_req_header(SOCK_MSG_SENDTO, s);
    sock_req_t *r = (sock_req_t *)(g_sreq + sizeof(mach_msg_header_t));
    uint32_t n = len > SOCK_MAX_DATA ? SOCK_MAX_DATA : len;
    if (n && buf) memcpy(r->data, buf, n);
    r->len = n;
    if (addr && addrlen) { uint32_t an = addrlen > 16 ? 16 : addrlen; memcpy(r->addr, addr, an); r->addr_len = an; }
    suki_status_t st = sock_ipc();
    if (st != 0) return st;
    if (out_nwritten) *out_nwritten = sock_resp()->result;
    return 0;
}

suki_status_t SukiSocketRecvFrom(suki_socket_t s, void *buf, uint32_t len,
                                   uint32_t flags, void *addr, uint32_t *addrlen,
                                   uint32_t *out_nread)
{
    (void)flags;
    sock_req_header(SOCK_MSG_RECVFROM, s);
    sock_req_t *r = (sock_req_t *)(g_sreq + sizeof(mach_msg_header_t));
    r->len = len;
    suki_status_t st = sock_ipc();
    if (st != 0) return st;
    sock_resp_t *rp = sock_resp();
    uint32_t n = rp->len > len ? len : rp->len;
    if (n && buf) memcpy(buf, rp->data, n);
    if (addr && addrlen && rp->addr_len) { uint32_t an = rp->addr_len > 16 ? 16 : rp->addr_len; memcpy(addr, rp->addr, an); *addrlen = rp->addr_len; }
    if (out_nread) *out_nread = n;
    return 0;
}

suki_status_t SukiSocketClose(suki_socket_t s)
{
    sock_req_header(SOCK_MSG_CLOSE, s);
    return sock_ipc();
}

suki_status_t SukiSocketGetSockName(suki_socket_t s, void *addr, uint32_t *addrlen)
{
    sock_req_header(SOCK_MSG_GETSOCKNAME, s);
    suki_status_t st = sock_ipc();
    if (st != 0) return st;
    sock_resp_t *rp = sock_resp();
    if (addr && addrlen && rp->addr_len) { uint32_t n = rp->addr_len > 16 ? 16 : rp->addr_len; memcpy(addr, rp->addr, n); *addrlen = rp->addr_len; }
    return 0;
}

suki_status_t SukiSocketGetPeerName(suki_socket_t s, void *addr, uint32_t *addrlen)
{
    sock_req_header(SOCK_MSG_GETPEERNAME, s);
    suki_status_t st = sock_ipc();
    if (st != 0) return st;
    sock_resp_t *rp = sock_resp();
    if (addr && addrlen && rp->addr_len) { uint32_t n = rp->addr_len > 16 ? 16 : rp->addr_len; memcpy(addr, rp->addr, n); *addrlen = rp->addr_len; }
    return 0;
}

suki_status_t SukiSocketShutdown(suki_socket_t s, int how)
{
    (void)how;
    sock_req_header(SOCK_MSG_SHUTDOWN, s);
    return sock_ipc();
}

suki_status_t SukiSocketSetSockOpt(suki_socket_t s, int level, int optname,
                                     const void *optval, uint32_t optlen)
{
    sock_req_header(SOCK_MSG_SETSOCKOPT, s);
    sock_req_t *r = (sock_req_t *)(g_sreq + sizeof(mach_msg_header_t));
    r->domain = (uint32_t)level; r->type = (uint32_t)optname;
    uint32_t n = optlen > 16 ? 16 : optlen;
    if (optval && n) memcpy(r->addr, optval, n);
    r->addr_len = n;
    return sock_ipc();
}

suki_status_t SukiSocketGetSockOpt(suki_socket_t s, int level, int optname,
                                     void *optval, uint32_t *optlen)
{
    sock_req_header(SOCK_MSG_GETSOCKOPT, s);
    sock_req_t *r = (sock_req_t *)(g_sreq + sizeof(mach_msg_header_t));
    r->domain = (uint32_t)level; r->type = (uint32_t)optname;
    uint32_t inlen = optlen ? *optlen : 0;
    uint32_t n = inlen > 16 ? 16 : inlen;
    r->addr_len = n;
    suki_status_t st = sock_ipc();
    if (st != 0) return st;
    sock_resp_t *rp = sock_resp();
    if (optval && optlen) { uint32_t m = rp->len > n ? n : rp->len; if (m) memcpy(optval, rp->data, m); *optlen = rp->len; }
    return 0;
}
