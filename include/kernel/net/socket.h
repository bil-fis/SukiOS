/*
 * include/kernel/net/socket.h
 * -----------------------------------------------------------------------------
 * 内核网络子系统（POSIX 网络 syscall 150..164 + fd 集成）接口。
 *
 * 架构：内核不直接解析任何网络协议，仅把 socket 操作翻译成 NS_PORT 的 IPC 消息
 * （与 fd.c 转发 FS_PORT 同构），由 net_server（用户态 lwIP）真正执行。这样符合
 * SukiOS「驱动/协议栈用户态化」的混合内核红线，内核只做 fd 表与语义翻译。
 *
 * 关键不变量（同 fd.c）：
 *   - 用户指针一律经 copy_from_user/copy_to_user，绝不直接解引用；
 *   - 所有 IPC 往返（会阻塞）在 fd 表锁外执行。
 */
#ifndef _SUKI_KERNEL_NET_SOCKET_H
#define _SUKI_KERNEL_NET_SOCKET_H

#include <kernel/types.h>
#include <kernel/fd.h>
#include <sukios/net.h>

/* 转发缓冲上限（须 >= 单条 SOCK 消息：mach_msg_header + sock_req_t/resp_t） */
#define NET_RPC_MAX (sizeof(mach_msg_header_t) + sizeof(sock_req_t) + 32)

/* POSIX 网络 syscall（150..164）分发表入口 */
uint64_t sys_net_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6);

/* 供 fd.c 集成：fd 类型为 FD_TYPE_SOCKET 时路由到这里 */
void         net_close_backend(fd_entry_t *e);                 /* fd_release_slot 调用 */
suki_ssize_t net_read(struct task *t, int fd, void *ubuf, size_t count);
suki_ssize_t net_write(struct task *t, int fd, const void *ubuf, size_t count);

/* 分配一个 socket fd（类型为 FD_TYPE_SOCKET，backend = net_server 句柄） */
int fd_socket(struct task *t, int handle);

/* 查询某个 socket fd 的就绪掩码（POSIX POLL* 位）。供 sys_poll/sys_select 在
 * 遇到 FD_TYPE_SOCKET 时调用：经 SOCK_MSG_POLL 问 net_server（lwIP 状态），
 * 返回 net_server 算好的掩码（SUKI_POLLIN/SUKI_POLLOUT/SUKI_POLLERR/SUKI_POLLNVAL）。
 * fd 非 socket 或查询失败时返回 0。 */
uint32_t net_poll(int fd, uint32_t want);

#endif /* _SUKI_KERNEL_NET_SOCKET_H */
