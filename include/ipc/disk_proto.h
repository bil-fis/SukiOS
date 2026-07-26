/*
 * include/ipc/disk_proto.h
 * -----------------------------------------------------------------------------
 * DISK_PORT IPC 协议（内核 disk-srv 与用户态服务共享的消息布局）。
 *
 * 请求（发往 DISK_PORT）：
 *   header.msgh_id         = DISK_MSG_READ
 *   header.msgh_local_port = 应答端口
 *   payload                = disk_read_req_t
 * 应答（发往请求方 local_port）：
 *   header.msgh_id = DISK_MSG_READ, payload = status(u64) + 扇区数据
 */
#ifndef _SUKI_IPC_DISK_PROTO_H
#define _SUKI_IPC_DISK_PROTO_H

#include <stdint.h>

#define DISK_MSG_READ      1
#define DISK_MSG_WRITE     2

/* 单次请求扇区数上限：受内联消息 3968B 限制（7*512=3584 + 头） */
#define DISK_MAX_SECTORS   7

typedef struct disk_read_req {
    uint64_t lba;
    uint32_t count;                 /* 1..DISK_MAX_SECTORS */
    uint32_t pad;
} disk_read_req_t;

typedef struct disk_read_resp {
    uint64_t status;                /* 0=OK, 非 0=错误 */
    /* 后随 count*512 字节扇区数据 */
} disk_read_resp_t;

/* 写请求：头部字段后紧随 count*512 字节待写数据 */
typedef struct disk_write_req {
    uint64_t lba;
    uint32_t count;                 /* 1..DISK_MAX_SECTORS */
    uint32_t pad;
    /* 后随 count*512 字节扇区数据 */
} disk_write_req_t;

typedef struct disk_write_resp {
    uint64_t status;                /* 0=OK, 非 0=错误 */
} disk_write_resp_t;

#endif /* _SUKI_IPC_DISK_PROTO_H */
