/*
 * include/ipc/fs_proto.h
 * -----------------------------------------------------------------------------
 * FS_PORT IPC 协议（Ring3 FS_SERVER 与内核 VFS 层 / 客户端共享）。
 *
 * 协议分两代，二者并存、互不干扰：
 *   【一代 · 路径式】FS_MSG_LIST / READ / READ_AT / READ_FILE / CREATE /
 *     MKDIR / WRITE / UNLINK / RENAME / TRUNCATE —— 早期接口，shell 等既有
 *     代码仍在使用（一次调用完成"打开+定位+IO+关闭"）。
 *   【二代 · 句柄式（POSIX 就绪）】FS_MSG_OPEN / CLOSE / READFD / WRITEFD /
 *     SEEKFD / FSTAT / FTRUNCATE / STAT / OPENDIR / READDIR / CLOSEDIR /
 *     MKDIR2 / UNLINK2 / RENAME2 / ACCESS —— 与 POSIX 的 open/read/write/
 *     lseek/stat/opendir/readdir/closedir 一一对应。内核 VFS 层
 *     （kernel/fs/fd.c）用它支撑完整的 POSIX 文件系统调用。
 *
 * 传输与容量约束：
 *   - 请求/应答均走 MACH 内联消息，单条总长 < MACH_MSG_INLINE_MAX(3968)；
 *   - 整文件读（execve）走 OOL 物理页零拷贝，上限 16 页（64 KiB）；
 *   - 路径长度上限 FS_PATH_MAX(256)；单次读写上限 FS_RW_MAX(3584)。
 *
 * 应答统一格式：fs_resp_t{status, length} + 可选数据；
 *   status 见 FS_OK / FS_ERR_*；length 为后随数据字节数。
 * 所有句柄式应答在 fs_resp_t 之后紧跟一个 fs_ret_t{value}（返回值），
 * 其后才是可选的数据负载（如 read 的文件内容）。
 */
#ifndef _SUKI_IPC_FS_PROTO_H
#define _SUKI_IPC_FS_PROTO_H

#include <stdint.h>

/* ============ 一代：路径式 ============ */
#define FS_MSG_LIST   1
#define FS_MSG_READ   2
#define FS_MSG_READ_FILE 3   /* 内核 execve 用：返回 OOL 携带的完整文件内容 */
#define FS_MSG_READ_AT   4   /* 分块读：从 offset 起读 length 字节 */
#define FS_MSG_CREATE    5   /* 创建空文件；负载 = NUL 结尾路径（已存在则幂等） */
#define FS_MSG_MKDIR     6   /* 创建目录；负载 = NUL 结尾路径 */
#define FS_MSG_WRITE     7   /* 写文件；负载 = fs_write_req_t + 文件名 + 数据 */
#define FS_MSG_UNLINK    8   /* 删除文件/目录；负载 = NUL 结尾路径 */
#define FS_MSG_RENAME    9   /* 重命名；负载 = fs_rename_req_t */
#define FS_MSG_TRUNCATE 10   /* 截断；负载 = fs_trunc_req_t + 文件名 */

/* ============ 二代：句柄式（POSIX 就绪）============ */
#define FS_MSG_OPEN      20  /* open(path, flags, mode) -> fs_ret_t(fd) */
#define FS_MSG_CLOSE     21  /* close(fd) */
#define FS_MSG_READFD    22  /* read(fd, length) -> ret(nread) + data */
#define FS_MSG_WRITEFD   23  /* write(fd, offset<0=当前位置, length, data) */
#define FS_MSG_SEEKFD    24  /* lseek(fd, offset, whence) -> ret(newpos) */
#define FS_MSG_FSTAT     25  /* fstat(fd) -> ret + fs_stat_t */
#define FS_MSG_FTRUNC    26  /* ftruncate(fd, size) */
#define FS_MSG_STAT      27  /* stat(path) -> ret + fs_stat_t */
#define FS_MSG_OPENDIR   28  /* opendir(path) -> ret(dd) */
#define FS_MSG_READDIR   29  /* readdir(dd) -> ret + fs_dirent_t */
#define FS_MSG_CLOSEDIR  30  /* closedir(dd) */
#define FS_MSG_MKDIR2    31  /* mkdir(path, mode) */
#define FS_MSG_UNLINK2   32  /* unlink/rmdir(path, isdir) */
#define FS_MSG_RENAME2   33  /* rename(old, new) */
#define FS_MSG_ACCESS    34  /* access(path, mode) */
#define FS_MSG_STATFS    35  /* statfs() -> ret + fs_statfs_t */
#define FS_MSG_SYNC      36  /* 刷写卷缓存 */
#define FS_MSG_UTIME     37  /* 设置文件时间；负载 = fs_utime_req_t + 路径 */
#define FS_MSG_CHMOD     38  /* 设置权限/属性；负载 = fs_chmod_req_t + 路径 */

/* 单条读/写数据上限：<= DISK_MAX_SECTORS*512(3584)，且必须 < MACH_MSG_INLINE_MAX(3968) */
#define FS_WRITE_MAX     3584
#define FS_READ_MAX      3584

/* 路径长度上限（含 NUL） */
#define FS_PATH_MAX      256

/* 服务下线通知：当某服务进程（fs-server 等）崩溃/退出时，内核会把其端口上
 * 排队的请求排干，并向每个请求的 msgh_local_port 回送一条本 id 的应答，
 * 负载为 fs_resp_t{status=FS_ERR_IO, length=0}，msgh_reserved 回带原请求 id。
 * 客户端收到后应立即放弃等待并回到可交互状态（防止永久阻塞 / 键盘失灵）。 */
#define MSG_ID_SERVICE_DOWN 101   /* 避开 FS_MSG_*(1..37) 与 MSG_ID_KEYCHAR(100) */

#define FS_OK         0
#define FS_ERR_IO     1
#define FS_ERR_NOENT  2
#define FS_ERR_TOOBIG 3
#define FS_ERR_EXIST  4    /* 已存在（O_EXCL 冲突） */
#define FS_ERR_ACCES  5    /* 权限/只读卷冲突 */
#define FS_ERR_NOTDIR 6    /* 路径分量不是目录 */
#define FS_ERR_ISDIR  7    /* 目标是目录（如以写方式 open 目录） */
#define FS_ERR_NOSPC  8    /* 卷空间不足 */
#define FS_ERR_INVAL  9    /* 参数非法 */
#define FS_ERR_BADF   10   /* 句柄无效 */
#define FS_ERR_NFILE  11   /* 句柄表耗尽 */

/* 应答负载上限（受内联消息限制） */
#define FS_DATA_MAX   3500

/* ===================== 通用结构 ===================== */

typedef struct fs_resp {
    uint32_t status;
    uint32_t length;              /* 后随数据字节数（含 fs_ret_t，若有） */
} fs_resp_t;

/* 句柄式应答的返回值块：紧跟 fs_resp_t。value 语义依具体消息：
 *   OPEN -> 文件句柄(>=0) 或负 errno；READFD -> 读取字节数；
 *   SEEKFD -> 新偏移；OPENDIR -> 目录句柄；READDIR -> 1=有条目 0=目录结束。 */
typedef struct fs_ret {
    int64_t value;
} fs_ret_t;

/* 一代：分块读请求 */
typedef struct fs_read_at_req {
    uint32_t offset;
    uint32_t length;
    /* 后随文件名字符串 */
} fs_read_at_req_t;

/* 一代：写请求 */
typedef struct fs_write_req {
    uint32_t offset;
    uint32_t length;
    /* 后随：NUL 结尾文件名 + length 字节数据 */
} fs_write_req_t;

/* 一代：截断请求 */
typedef struct fs_trunc_req {
    uint32_t size;
    /* 后随文件名字符串 */
} fs_trunc_req_t;

/* 一代：重命名请求 */
typedef struct fs_rename_req {
    char old_name[64];
    char new_name[64];
} fs_rename_req_t;

/* ===================== 二代：句柄式结构 ===================== */

/* FS_MSG_OPEN 请求：flags/mode 用 POSIX 语义位（见 sukios/posix.h 的
 * SUKI_O_* / SUKI_S_*）。负载 = 本结构 + NUL 结尾路径。 */
typedef struct fs_open_req {
    int32_t  flags;               /* SUKI_O_* 组合 */
    uint32_t mode;                /* 创建时的权限位（当前仅作登记） */
} fs_open_req_t;

/* FS_MSG_READFD 请求 */
typedef struct fs_read_req {
    uint32_t fd;                  /* FS_SERVER 侧文件句柄 */
    uint32_t length;              /* 期望读取字节数（会被截断到 FS_READ_MAX） */
} fs_read_req_t;

/* FS_MSG_WRITEFD 请求：负载 = 本结构 + length 字节数据 */
typedef struct fs_write_fd_req {
    uint32_t fd;
    uint32_t length;
    int64_t  offset;              /* <0 表示从当前文件位置写（POSIX write 语义） */
} fs_write_fd_req_t;

/* FS_MSG_SEEKFD 请求 */
typedef struct fs_seek_req {
    uint32_t fd;
    int32_t  whence;              /* SUKI_SEEK_SET/CUR/END */
    int64_t  offset;
} fs_seek_req_t;

/* FS_MSG_FTRUNC 请求 */
typedef struct fs_ftrunc_req {
    uint32_t fd;
    int64_t  length;
} fs_ftrunc_req_t;

/* FS_MSG_CLOSE / FSTAT / OPENDIR / CLOSEDIR / READDIR 请求：单个句柄 */
typedef struct fs_fd_req {
    uint32_t fd;
} fs_fd_req_t;

/* FS_MSG_UNLINK2 请求：is_dir=1 走 rmdir 语义 */
typedef struct fs_unlink_req {
    uint32_t is_dir;
    /* 后随 NUL 结尾路径 */
} fs_unlink_req_t;

/* FS_MSG_ACCESS / MKDIR2 / STAT 请求：mode 之后紧跟 NUL 结尾路径 */
typedef struct fs_path_mode_req {
    int32_t mode;
    /* 后随 NUL 结尾路径 */
} fs_path_mode_req_t;

/* FS_MSG_UTIME 请求：负载 = 本结构 + NUL 结尾路径 */
typedef struct fs_utime_req {
    int64_t atime;                /* POSIX 时间（秒）；<0 表示不修改 */
    int64_t mtime;
} fs_utime_req_t;

/* FS_MSG_CHMOD 请求：负载 = 本结构 + NUL 结尾路径。
 * mode 用 POSIX 权限位；FAT32 只能承载「只读」这一位，映射规则见
 * fs_server.c::handle_chmod（写位全无 -> AM_RDO，否则清 AM_RDO）。 */
typedef struct fs_chmod_req {
    uint32_t mode;
    uint32_t reserved;
} fs_chmod_req_t;

/* 文件属性（fstat/stat 应答数据；字段语义同 POSIX struct stat） */
typedef struct fs_stat {
    uint64_t dev;
    uint64_t ino;
    uint32_t mode;                /* SUKI_S_IF* | 权限位 */
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint64_t rdev;
    int64_t  size;
    int64_t  blksize;
    int64_t  blocks;
    int64_t  atime;               /* 秒 */
    int64_t  mtime;
    int64_t  ctime;
} fs_stat_t;

/* 目录项（readdir 应答数据） */
typedef struct fs_dirent {
    uint64_t ino;
    uint8_t  type;                /* SUKI_DT_* */
    uint8_t  _pad[7];
    char     name[256];           /* NUL 结尾 */
} fs_dirent_t;

/* 卷信息（statfs 应答数据） */
typedef struct fs_statfs {
    uint64_t type;
    uint64_t bsize;
    uint64_t blocks;
    uint64_t bfree;
    uint64_t files;
    uint64_t ffree;
    uint64_t namelen;
} fs_statfs_t;

#endif /* _SUKI_IPC_FS_PROTO_H */
