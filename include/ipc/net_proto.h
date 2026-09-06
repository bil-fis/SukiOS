/*
 * include/ipc/net_proto.h
 * -----------------------------------------------------------------------------
 * NET_PORT 的 Mach IPC 协议定义（仿 include/ipc/disk_proto.h 的分层约定）。
 *
 * 分层（与磁盘路径同构，符合混合内核红线）：
 *   应用/服务 --IPC--> [上层 socket 服务（lwIP）] --NET_PORT IPC--> Ring0 e1000 驱动
 *
 * 红线：Ring0 的 e1000 驱动只做「原始以太网帧」收发，绝不解析 IP/TCP/UDP；
 *       协议栈逻辑全部在用户态网络服务（lwIP）中，与 FAT32 在 FS_SERVER 同理。
 *
 * 调用关系：kernel/drivers/e1000.c::net_srv_task 响应本协议的请求。
 */
#ifndef _SUKI_IPC_NET_PROTO_H
#define _SUKI_IPC_NET_PROTO_H

#include <kernel/types.h>

/* 知名端口：0=sentinel，1=DISK，2=FS，3=NET（新增） */
#define NET_PORT            3

/* 消息 ID */
#define NET_MSG_GET_MAC     1   /* 取 MAC 地址 + 链路状态 */
#define NET_MSG_SEND        2   /* 发送一帧（数据 inline 跟在请求头后） */
#define NET_MSG_RECV        3   /* 取一帧（无帧时阻塞轮询等待，超时返回 0 长度） */

/* 标准以太网帧上限（不含 CRC；驱动已用 RCTL.SECRC 剥离 CRC） */
#define NET_MAX_FRAME       1514

/* NET_MSG_GET_MAC 应答体 */
typedef struct net_mac_resp {
    uint32_t status;        /* 0=成功，非 0=失败（无网卡） */
    uint8_t  mac[6];
    uint8_t  link_up;       /* 1=链路已连接 */
    uint8_t  pad;
} net_mac_resp_t;

/* NET_MSG_SEND 请求体（其后紧跟 length 字节帧数据） */
typedef struct net_send_req {
    uint32_t length;        /* 帧长度，1..NET_MAX_FRAME */
} net_send_req_t;

/* NET_MSG_SEND 应答体 */
typedef struct net_send_resp {
    uint32_t status;        /* 0=已提交发送，非 0=失败 */
} net_send_resp_t;

/* NET_MSG_RECV 应答体（其后紧跟 length 字节帧数据） */
typedef struct net_recv_resp {
    uint32_t status;        /* 0=成功（length 可能仍为 0 表示超时无帧） */
    uint32_t length;        /* 实际帧长度；0 表示本次未收到帧 */
} net_recv_resp_t;

#endif /* _SUKI_IPC_NET_PROTO_H */
