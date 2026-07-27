/*
 * include/ipc/port.h
 * -----------------------------------------------------------------------------
 * Mach 式 IPC 端口（手册第 7 章）。
 *
 * 模型：
 *   - 内核维护全局端口表 kernel_port_t[]，端口号即索引（含代次校验）。
 *   - 小消息 (<4KB)：内核 memcpy 两次拷贝（发送方->内核队列->接收方）。
 *   - 大消息（像素缓冲等）：OOL 描述符仅携带物理页号列表，接收时重映射
 *     到接收方地址空间，引用计数 +1，实现零拷贝。
 *   - 知名端口 (well-known ports)：DISK_PORT / FS_PORT / DISPLAY_PORT /
 *     INPUT_PORT / CONSOLE_PORT，供服务注册与查找。
 *
 * 调用关系：sys_mach_msg -> ipc_send/ipc_recv -> port 队列；
 *           内核服务（DISK_PORT）由内核线程直接收发。
 */
#ifndef _SUKI_IPC_PORT_H
#define _SUKI_IPC_PORT_H

#include <kernel/types.h>
#include <kernel/task.h>

/* ---- 消息头（用户/内核共享 ABI）---- */
#define MACH_MSG_INLINE_MAX   3968          /* 小消息内联负载上限 (<4KB 含头) */
#define MACH_MSG_OOL_MAX_PAGES 16           /* 单条 OOL 消息最多页数 */

/* msgh_bits 标志 */
#define MACH_MSGH_BITS_OOL    (1U << 31)    /* 携带 OOL 页描述符 */

typedef struct mach_msg_header {
    uint32_t msgh_bits;                     /* 标志位 */
    uint32_t msgh_size;                     /* 总大小（含头） */
    uint32_t msgh_remote_port;              /* 目的端口 */
    uint32_t msgh_local_port;               /* 回复端口（可为 0） */
    uint32_t msgh_id;                       /* 消息 ID（协议自定义） */
    uint32_t msgh_reserved;
} mach_msg_header_t;

/* OOL 描述符：紧跟消息头之后（内核在投递时改写为接收方虚拟地址） */
typedef struct mach_ool_desc {
    uint64_t address;                       /* 发送:用户VA / 接收:映射后VA */
    uint64_t size;                          /* 字节数 */
} mach_ool_desc_t;

/* mach_msg option */
#define MACH_SEND_MSG   0x1
#define MACH_RECV_MSG   0x2

/* 返回码 */
#define MACH_MSG_SUCCESS        0
#define MACH_SEND_INVALID_DEST  0x10000003UL
#define MACH_SEND_TOO_LARGE     0x10000004UL
#define MACH_RCV_INVALID_NAME   0x10002002UL
#define MACH_RCV_TOO_LARGE      0x10004004UL
#define MACH_RCV_TIMED_OUT      0x10004003UL
/* OOL 接收窗口耗尽（H4）：无更多映射区间且复用链表无法满足需求 */
#define MACH_RCV_NO_SPACE       0x10005005UL
#define MACH_INVALID_ARGUMENT   0x10000002UL
/* 消息队列已满（M5）：背压信号，发送方应限流/重试 */
#define MACH_SEND_NO_BUFFER     0x10000005UL

/* ---- 知名端口号（1 起始；0 = PORT_NULL）---- */
#define PORT_NULL       0
#define DISK_PORT       1               /* 内核 ATA 服务（手册红线：唯一内核态驱动） */
#define FS_PORT         2               /* FAT32 文件系统服务 */
#define DISPLAY_PORT    3               /* 显示合成服务 */
#define INPUT_PORT      4               /* 输入服务 */
#define CONSOLE_PORT    5               /* 内核控制台输出服务 */
#define SHELL_PORT      6               /* Shell 接收（键盘字符 + FS 应答） */
#define FS_REPLY_PORT   7               /* FS_SERVER 接收磁盘应答 */
#define APP_PORT        8               /* 通用客户端(独立 app)可认领的应答端口 */
#define PORT_FIRST_DYN  9               /* 动态分配起始 */
#define PORT_MAX        64
/* 单端口消息队列长度上限（M5 修复）：防止失控/恶意任务狂发消息耗尽内核堆，
 * 超出即拒绝投递并返回 MACH_SEND_NO_BUFFER 形成背压。 */
#define PORT_QUEUE_MAX  64

/* ---- 内核端口对象 ---- */
typedef struct kernel_msg {
    struct kernel_msg *next;
    uint32_t size;                          /* 内联部分大小（含头） */
    bool     has_ool;
    uint64_t ool_pages[MACH_MSG_OOL_MAX_PAGES]; /* 物理页号列表 */
    uint32_t ool_page_count;
    uint64_t ool_size;                      /* OOL 原始字节数 */
    uint64_t ool_mapped_va;                 /* 接收方映射后的虚拟基址（A3 回收用） */
    uint8_t  data[];                        /* 内联数据（头+负载） */
} kernel_msg_t;

/* OOL 映射链表节点：挂在接收方 task->ool_maps 上，任务退出时解除映射并减引用 */
typedef struct ool_map_node {
    struct ool_map_node *next;
    uint64_t va;                            /* 接收方地址空间中的映射基址 */
    uint32_t count;                         /* 页数 */
    uint64_t pages[MACH_MSG_OOL_MAX_PAGES]; /* 共享物理页 */
} ool_map_node_t;

typedef struct kernel_port {
    bool         in_use;
    uint32_t     name;                      /* 端口号 */
    kernel_msg_t *queue_head, *queue_tail;  /* 待收消息队列 */
    uint32_t     queue_len;
    /* 等待接收的任务队列（FIFO，A4 项：取代单 waiter 指针） */
    task_t      *waiter_head, *waiter_tail;
    task_t      *owner;                     /* 拥有接收权的任务（A2 项） */
    task_t      *send_owner;                /* 唯一允许发送的任务；NULL=任何人可发 */
} kernel_port_t;

void      ipc_init(void);
uint32_t  port_allocate(task_t *owner);                 /* 返回端口号，0=失败 */
void      port_set_owner(uint32_t name, task_t *owner);
void      port_grant_send(uint32_t name, task_t *owner); /* 授权某任务向内核端口发送 */
uint64_t  port_claim(uint32_t name);                    /* 用户态认领端口 recv 权 */
void      port_release_owner(task_t *t);                /* 任务退出：释放其认领的端口所有权 */
void      port_reap_ool(task_t *t);                     /* 回收任务持有的 OOL 映射 */
kernel_port_t *port_lookup(uint32_t name);

/* 内核侧收发（供内核服务线程使用；msg 为内核缓冲） */
uint64_t  ipc_send_kernel(uint32_t dest, const void *msg, uint32_t size);
uint64_t  ipc_recv_kernel(uint32_t port_name, void *buf, uint32_t buf_size,
                          uint32_t *out_size, bool block);

/* 内核侧接收 OOL 消息：把 inline 部分拷入 inline_buf，把 OOL 物理页内容直接
 * 拷入 ool_buf（不经任何用户地址空间映射，仅消费 OOL 引用计数），供内核
 * 读文件等不需要把数据映射到用户空间的场景（如 execve 加载 ELF）。 */
uint64_t  ipc_recv_ool_kernel(uint32_t port_name, void *inline_buf,
                              uint32_t inline_cap, uint32_t *inline_out,
                              void *ool_buf, uint32_t ool_cap,
                              uint32_t *ool_out, bool block);
/* 释放一个动态分配的端口（execve 临时申请的应答端口用完即释放） */
void      port_free(uint32_t name);

/* syscall 入口（强符号覆盖 syscall.c 中的 weak 占位） */
uint64_t  sys_mach_msg(uint64_t msg_uptr, uint64_t option,
                       uint64_t send_size, uint64_t recv_limit, uint64_t port);

#endif /* _SUKI_IPC_PORT_H */
