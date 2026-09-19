/*
 * include/kernel/fd.h
 * -----------------------------------------------------------------------------
 * 内核文件描述符（VFS）层接口。
 *
 * 分层：POSIX syscall(sys_posix.c) -> 本层 fd.c -> Mach IPC -> Ring3 FS_SERVER。
 * 本层只做「fd 表 + 语义翻译 + 用户指针安全校验」，不含任何文件系统格式
 * 解析（FAT32 完全由用户态 FS_SERVER 通过 FatFs 完成，符合混合内核红线）。
 *
 * 关键不变量（改动本层时必须遵守）：
 *   1) 【锁内绝不阻塞】fd.c 的 g_fd_lock 是自旋锁；任何会阻塞的操作
 *      （向 FS_SERVER 发 CLOSE 等 IPC 往返）必须在【解锁后】执行。故槽的
 *      「摘除」与「释放」被拆成两个函数（*_locked / fd_release_slot）。
 *   2) 【per-CPU 临时缓冲】syscall 路径的读写暂存缓冲必须是 per-CPU 的：
 *      一次 RPC 可能因等待应答而 schedule 让出，同核另一任务若复用同一
 *      静态缓冲会互踩数据（真实正确性缺陷，非性能问题）。
 *   3) 【绝不信任外部长度】FS_SERVER 应答中的 length/value 一律先与
 *      实际收到的消息长度比对，越界即判 -EIO，不拷贝、不信任。
 *
 * 调用关系：
 *   kmain -> posix_init() -> fd_init()
 *   sched.c::task_create_user_args -> fd_install_stdio()
 *   sched.c::task_exit_current     -> fd_exit_task()
 *   sys_posix.c::sys_fork          -> fd_fork_clone()
 *   sys_posix.c::sys_open/read/... -> fd_open/fd_read/...
 */
#ifndef _SUKI_KERNEL_FD_H
#define _SUKI_KERNEL_FD_H

#include <kernel/types.h>
#include <sukios/posix.h>

#define SUKI_KERNEL_BUILD 1

struct task;

/* 全局 fd 槽池容量（所有进程共享槽池；每进程最多占 SUKI_FD_MAX 个） */
#define FD_SLOT_MAX      128

/* 管道环形缓冲大小（字节） */
#define FD_PIPE_BYTES    4096

/* fd 类型 */
#define FD_TYPE_NONE   0
#define FD_TYPE_FILE   1    /* 普通文件（后端 = FS_SERVER 文件句柄） */
#define FD_TYPE_DIR    2    /* 目录流（后端 = FS_SERVER 目录句柄） */
#define FD_TYPE_TTY    3    /* 控制台（内核本地实现） */
#define FD_TYPE_PIPE   4    /* 管道（内核本地环形缓冲） */
#define FD_TYPE_SOCKET 5    /* 网络 socket（后端 = net_server 分配的 sock handle） */

/* fd 后端（与 VFS 后端一一对应）。open 时由 VFS 路由确定；
 * read/write/lseek/fstat/close 据此分派到 FS_PORT 或内核内建后端。 */
#define FD_BACKEND_DISK  0   /* FS_PORT 的 Ring3 FS_SERVER(FatFs) —— 默认 */
#define FD_BACKEND_TMPFS 1   /* 内核态 tmpfs（/tmp、/run） */
#define FD_BACKEND_DEVFS 2   /* 内核态 devfs（/dev） */
#define FD_BACKEND_ISO   3   /* 内核态 ISO9660（仅光盘启动、脱离硬盘时接管 '/'） */

/* 前端 fd 表项（全局槽池中的一个槽） */
typedef struct fd_entry {
    uint32_t type;          /* FD_TYPE_* */
    int32_t  flags;         /* SUKI_O_* 打开标志（含访问模式） */
    int      backend;       /* 文件/目录后端句柄（DISK=TMPFS=DEVFS 各自的内部句柄）；
                             * 管道时 = peer 槽号 */
    uint8_t  vfs_backend;   /* FD_BACKEND_*：标识该 fd 的数据来自哪个 VFS 后端 */
    uint8_t  _pad[3];
    uint32_t refcount;      /* 引用计数（fork/dup 共享） */
    uint64_t offset;        /* 文件偏移（TTY/管道未用）；目录时作游标计数 */

    /* 管道：共享环形缓冲对象（带独立引用计数，两端各持一份） */
    struct fd_pipe *pipe;

    char    *path;          /* 打开时的路径副本（诊断 / 未来 getcwd 用） */

    /* SukiNative FILE 对象独占标记：由 SukiNative SYS_SUKI_FILE_OPEN 打开的 fd 置 true。
     * 任务退出时 fd_exit_task 跳过此类槽（不清 fds、不降引用、不释放），改由持有它的
     * SukiNative FILE 对象在引用归零（显式 CLOSE 或任务退出回收句柄表）时统一调
     * fd_close 释放，避免任务退出路径与对象销毁路径对同一 fd 双重关闭。 */
    bool     suki_owned;
} fd_entry_t;

/* 管道对象：读写两端共享，独立引用计数，归零才释放 */
typedef struct fd_pipe {
    uint32_t refs;
    uint32_t head;          /* 写位置 */
    uint32_t tail;          /* 读位置 */
    uint32_t count;         /* 当前有效字节数 */
    uint8_t  data[FD_PIPE_BYTES];
} fd_pipe_t;

void fd_init(void);

/* 为 Ring3 任务安装 stdin/stdout/stderr（三个 TTY fd）。成功返回 0。 */
int  fd_install_stdio(struct task *t);

/* 任务退出：释放其全部 fd（引用归零者关闭后端）。 */
void fd_exit_task(struct task *t);

/* fork：子进程复制父的 fd 表（共享槽，refcount++，POSIX 共享偏移语义）。 */
int  fd_fork_clone(struct task *child, const struct task *parent);

/* 取 fd 对应槽（不增引用；调用方保证不跨越可能导致任务退出的阻塞点）。 */
fd_entry_t *fd_get(struct task *t, int fd);

/* 关闭后端（向 FS_SERVER 发 CLOSE/CLOSEDIR）。会阻塞，严禁持 g_fd_lock 调用。 */
void fd_close_backend(fd_entry_t *e);

/* 解锁态释放一个已被摘除的槽（引用归零则关后端并释放内存）。会阻塞。 */
void fd_release_slot(int slot);

/* ---- POSIX 文件操作（返回 0/正值成功，负 errno 失败） ---- */
int          fd_open(struct task *t, const char *path, int flags, uint32_t mode);
int          fd_close(struct task *t, int fd);
suki_ssize_t fd_read(struct task *t, int fd, void *ubuf, size_t count);
/* 内核态读：数据直接写入内核缓冲 kbuf（不经 copy_to_user）。
 * 仅供内核自身把 ELF 映像等载入内核空间（如 SukiNative PROC_CREATE）使用。
 * 仅支持常规文件后端（DISK/TMPFS/DEVFS）；TTY/PIPE/DIR 返回 -EINVAL。 */
suki_ssize_t fd_read_kern(struct task *t, int fd, void *kbuf, size_t count);
suki_ssize_t fd_write(struct task *t, int fd, const void *ubuf, size_t count);
suki_off_t   fd_lseek(struct task *t, int fd, suki_off_t off, int whence);
int          fd_fstat(struct task *t, int fd, suki_stat_t *out);
int          fd_stat(const char *path, suki_stat_t *out);
int          fd_opendir(struct task *t, const char *path);
int          fd_readdir(struct task *t, int fd, suki_dirent_t *out);
int          fd_unlink(const char *path);
int          fd_rmdir(const char *path);
int          fd_mkdir(const char *path, uint32_t mode);
int          fd_access(const char *path, int mode);
int          fd_rename(const char *oldp, const char *newp);
int          fd_truncate(const char *path, suki_off_t len);
int          fd_ftruncate(struct task *t, int fd, suki_off_t len);
int          fd_dup(struct task *t, int oldfd);
int          fd_dup2(struct task *t, int oldfd, int newfd);
int          fd_pipe(struct task *t, int fds[2]);
int          fd_fcntl(struct task *t, int fd, int cmd, int64_t arg);
int          fd_isatty(struct task *t, int fd);
int          fd_statfs(suki_statfs_t *out);
int          fd_sync(void);
int          fd_chmod(const char *path, uint32_t mode);
int          fd_utimes(const char *path, int64_t atime, int64_t mtime);

#endif /* _SUKI_KERNEL_FD_H */
