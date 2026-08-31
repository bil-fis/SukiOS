/*
 * include/ipc/disk_proto.h
 * -----------------------------------------------------------------------------
 * DISK_PORT IPC 协议（内核 disk-srv 与用户态服务共享的消息布局）。
 *
 * 请求（发往 DISK_PORT）：
 *   header.msgh_id         = DISK_MSG_READ / DISK_MSG_WRITE
 *   header.msgh_local_port = 应答端口
 *   inline payload         = disk_read_req_t / disk_write_req_t（+写数据内联）
 *
 * 应答：
 *   DISK_MSG_READ  —— inline: status(u64)；数据走 OOL 物理页（count*512 字节，
 *                     上限 DISK_MAX_SECTORS*512=32KiB，= 8 个物理页）。接收方用
 *                     ipc_recv_ool_kernel 取回，结束 pmm_decref 回收引用。
 *   DISK_MSG_WRITE —— inline: status(u64) + 写请求内联数据（写请求仍走 inline，
 *                     因为写数据来自发起方用户缓冲，内联 ≤ 7*512 足够；本优化
 *                     只针对"读大文件"场景，写路径保持简单）。
 */
#ifndef _SUKI_IPC_DISK_PROTO_H
#define _SUKI_IPC_DISK_PROTO_H

#include <stdint.h>
#include <kernel/types.h>   /* PAGE_SIZE */

#define DISK_MSG_READ      1
#define DISK_MSG_WRITE     2

/* 单次读请求扇区数上限：提升为 64（32KiB/批）。
 * 读应答改用 OOL 物理页传输，突破原内联 3968B 上限（原本只能 7 扇区），
 * 一次 IPC 即可回传 64 扇区，大幅减少大文件读取的 IPC 往返次数。 */
#define DISK_MAX_SECTORS   64
#define DISK_OOL_MAX_PAGES ((DISK_MAX_SECTORS * 512 + PAGE_SIZE - 1) / PAGE_SIZE) /* 8 */

/* 单次写请求扇区数上限：保持 7（内联，≤3968B）。
 * 写场景非本次优化重点，且写数据来自发起方用户缓冲，内联 ≤ 7*512 足够；
 * 提升写批上限需另走 OOL 发送，留待后续。 */
#define DISK_MAX_WRITE_SECTORS   7

typedef struct disk_read_req {
    uint64_t lba;
    uint32_t count;                 /* 1..DISK_MAX_SECTORS */
    uint32_t pad;
} disk_read_req_t;

typedef struct disk_read_resp {
    uint64_t status;                /* 0=OK, 非 0=错误（OOL 数据长度由 msgh 同批的 ool_size 决定） */
} disk_read_resp_t;

/* 写请求：头部字段后紧随 count*512 字节待写数据（内联，≤ DISK_MAX_SECTORS*512 让
 * 写请求过大时 disk-srv 仍会被 ipc_send_kernel 限制；写场景并非优化重点，暂不 OOL）。 */
typedef struct disk_write_req {
    uint64_t lba;
    uint32_t count;                 /* 1..DISK_MAX_SECTORS */
    uint32_t pad;
    /* 后随 count*512 字节扇区数据（内联） */
} disk_write_req_t;

typedef struct disk_write_resp {
    uint64_t status;                /* 0=OK, 非 0=错误 */
} disk_write_resp_t;

#endif /* _SUKI_IPC_DISK_PROTO_H */
