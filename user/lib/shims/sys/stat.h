/*
 * user/lib/shims/sys/stat.h
 * -----------------------------------------------------------------------------
 * SukiOS 用户态标准 <sys/stat.h>（最小子集，供 libcurl 等第三方库包含）。
 *
 * struct stat 与 user/lib/libc.h 中定义保持同一布局；两处用同一守卫宏
 * _SUKI_STRUCT_STAT_DEFINED 避免重复定义。
 */
#ifndef _SUKI_SHIM_SYS_STAT_H
#define _SUKI_SHIM_SYS_STAT_H

#include <sys/types.h>

/* ---- 文件类型 / 权限模式位 ---- */
#define S_IFMT   0170000
#define S_IFSOCK 0140000
#define S_IFLNK  0120000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000
#define S_ISUID  0004000
#define S_ISGID  0002000
#define S_ISVTX  0001000
#define S_IRWXU  00700
#define S_IRUSR  00400
#define S_IWUSR  00200
#define S_IXUSR  00100
#define S_IRWXG  00070
#define S_IRGRP  00040
#define S_IWGRP  00020
#define S_IXGRP  00010
#define S_IRWXO  00007
#define S_IROTH  00004
#define S_IWOTH  00002
#define S_IXOTH  00001

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)

/* 传统秒级时间戳成员名：本 libc 的 struct stat 以纳秒拆分字段存储，
 * 这里按传统名做成员别名（与 glibc 语义一致：tv_sec 部分）。 */
#ifndef st_atime
#define st_atime  st_atim_sec
#endif
#ifndef st_mtime
#define st_mtime  st_mtim_sec
#endif
#ifndef st_ctime
#define st_ctime  st_ctim_sec
#endif
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

/* ---- struct stat（与 libc.h 共用守卫） ---- */
#ifndef _SUKI_STRUCT_STAT_DEFINED
#define _SUKI_STRUCT_STAT_DEFINED
struct stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    int64_t  st_atim_sec;
    int64_t  st_atim_nsec;
    int64_t  st_mtim_sec;
    int64_t  st_mtim_nsec;
    int64_t  st_ctim_sec;
    int64_t  st_ctim_nsec;
};
#endif

/* ---- 函数（实现见 user/lib/unistd.c / libc） ---- */
int stat(const char *path, struct stat *buf);
int fstat(int fd, struct stat *buf);
int lstat(const char *path, struct stat *buf);
int chmod(const char *path, mode_t mode);
int fchmod(int fd, mode_t mode);
int mkdir(const char *path, mode_t mode);
int umask(mode_t mask);

#endif /* _SUKI_SHIM_SYS_STAT_H */
