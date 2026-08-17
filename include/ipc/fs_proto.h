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

/* ---- P1-2：写路径 ---- */
#define FS_MSG_CREATE    5   /* 创建空文件；负载 = NUL 结尾路径（已存在则幂等） */
#define FS_MSG_MKDIR     6   /* 创建目录；负载 = NUL 结尾路径 */
#define FS_MSG_WRITE     7   /* 写文件；负载 = fs_write_req_t + 文件名 + length 字节数据 */
#define FS_MSG_UNLINK    8   /* 删除文件/目录；负载 = NUL 结尾路径 */
#define FS_MSG_RENAME    9   /* 重命名；负载 = fs_rename_req_t（old_name + new_name） */
#define FS_MSG_TRUNCATE 10   /* 截断；负载 = fs_trunc_req_t + 文件名 */

/* 单条写请求内联数据上限：<= DISK_MAX_SECTORS*512(3584)，且必须 < MACH_MSG_INLINE_MAX(3968) */
#define FS_WRITE_MAX     3584

/* 服务下线通知：当某服务进程（fs-server 等）崩溃/退出时，内核会把其端口上
 * 排队的请求排干，并向每个请求的 msgh_local_port 回送一条本 id 的应答，
 * 负载为 fs_resp_t{status=FS_ERR_IO, length=0}，msgh_reserved 回带原请求 id。
 * 客户端收到后应立即放弃等待并回到可交互状态（防止永久阻塞 / 键盘失灵）。 */
#define MSG_ID_SERVICE_DOWN 101   /* 避开 FS_MSG_*(1..10) 与 MSG_ID_KEYCHAR(100) */

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

/* FS_MSG_WRITE 请求负载：紧跟消息头，其后为 NUL 结尾的文件名，再其后为
 * length 字节待写数据（数据必须紧邻文件名，中间无填充）。 */
typedef struct fs_write_req {
    uint32_t offset;             /* 文件内起始字节偏移（0=自文件头覆盖写） */
    uint32_t length;             /* 数据字节数（<= FS_WRITE_MAX） */
    /* 后随：NUL 结尾文件名 + length 字节数据 */
} fs_write_req_t;

/* FS_MSG_TRUNCATE 请求负载：紧跟消息头，其后为 NUL 结尾的文件名。 */
typedef struct fs_trunc_req {
    uint32_t size;               /* 目标文件大小（字节） */
    /* 后随文件名字符串 */
} fs_trunc_req_t;

/* FS_MSG_RENAME 请求负载：old_name 与 new_name 均为 NUL 结尾路径。 */
typedef struct fs_rename_req {
    char old_name[64];
    char new_name[64];
} fs_rename_req_t;

#endif /* _SUKI_IPC_FS_PROTO_H */
