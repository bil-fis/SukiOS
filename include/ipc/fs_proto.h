/*
 * include/ipc/fs_proto.h
 * -----------------------------------------------------------------------------
 * FS_PORT IPC 协议（Ring3 FS_SERVER 与客户端共享）。
 *
 * 请求（发往 FS_PORT）：
 *   msgh_id = FS_MSG_LIST：列根目录；无负载
 *   msgh_id = FS_MSG_READ：读文件；负载 = NUL 结尾文件名 (8.3 大写)
 *   msgh_local_port = 应答端口
 * 应答（发往请求方端口，msgh_id 原样返回）：
 *   负载 = fs_resp_t + 数据（LIST=文本；READ=文件内容）
 */
#ifndef _SUKI_IPC_FS_PROTO_H
#define _SUKI_IPC_FS_PROTO_H

#include <stdint.h>

#define FS_MSG_LIST   1
#define FS_MSG_READ   2
#define FS_MSG_READ_FILE 3   /* 内核 execve 用：返回 OOL 携带的完整文件内容 */
#define FS_MSG_READ_AT   4   /* 分块读：从 offset 起读 length 字节（大文件流式播放用） */

#define FS_OK         0
#define FS_ERR_IO     1
#define FS_ERR_NOENT  2
#define FS_ERR_TOOBIG 3

/* 应答负载上限（受内联消息限制） */
#define FS_DATA_MAX   3500

typedef struct fs_resp {
    uint32_t status;
    uint32_t length;              /* 后随数据字节数 */
} fs_resp_t;

/* FS_MSG_READ_AT 请求负载：紧跟消息头，其后为 NUL 结尾的 8.3 文件名/路径。
 * 响应为 fs_resp_t + data（data 长度 = min(length, 剩余, FS_DATA_MAX)）；
 * 到达文件末尾时 length 可能小于请求值，length=0 表示 EOF。 */
typedef struct fs_read_at_req {
    uint32_t offset;             /* 文件内起始字节偏移 */
    uint32_t length;             /* 期望读取字节数（会被截断到 FS_DATA_MAX） */
    /* 后随文件名字符串 */
} fs_read_at_req_t;

#endif /* _SUKI_IPC_FS_PROTO_H */
