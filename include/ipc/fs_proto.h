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

#endif /* _SUKI_IPC_FS_PROTO_H */
