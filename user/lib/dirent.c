/*
 * user/lib/dirent.c
 * -----------------------------------------------------------------------------
 * 目录流实现：opendir / readdir / closedir / telldir / seekdir / rewinddir。
 *
 * 后端全部走内核 POSIX syscall 层（posix.h 定义）：
 *   SYS_OPENDIR  (51)  -> 打开目录，返回内核 fd（<0 为 -errno）
 *   SYS_READDIR  (52)  -> 读一条目录项到 suki_dirent_t 缓冲
 *   SYS_CLOSEDIR (53)  -> 关闭目录 fd
 *   SYS_TELLDIR  (87)  -> 取当前目录流位置（kernel 维护的偏移）
 *   SYS_SEEKDIR  (88)  -> 定位目录流到指定位置
 *
 * 关键设计：
 *   * libc 的 `struct dirent` 与内核的 `suki_dirent_t` 字段/布局完全一致
 *     （d_ino/d_off/d_reclen/d_type/_pad/d_name，共 280 字节），因此
 *     SYS_READDIR 可直接写入用户态 struct dirent 缓冲，无需逐字段拷贝。
 *   * DIR 是不透明句柄，内部只保存内核 fd（open fd）+ 当前 readdir 游标。
 *   * 每个 DIR 持有一个静态结果缓冲（g_ent），readdir 返回其地址；由于
 *     经典 readdir 语义即「返回的 struct dirent * 在下次 readdir 前有效」，
 *     这里用每 DIR 独立的缓冲即可，符合 POSIX 弱约定。
 *   * 内核 readdir 在目录末尾返回 0 且不清缓冲；本层据此返回 NULL。
 */
#include "libc.h"
#include <sukios/posix.h>

#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

struct DIR {
    int   fd;        /* 内核 opendir 返回的 fd，<0 表示已关闭/无效 */
    long  pos;       /* telldir/seekdir 用的目录流偏移（由内核维护） */
    int   eof;       /* 是否已到目录末尾 */
    struct dirent ent; /* readdir 复用缓冲 */
};

DIR *opendir(const char *name)
{
    if (!name) { errno = EFAULT; return NULL; }
    long r = suki_syscall1(SYS_OPENDIR, (uint64_t)name);
    if (r < 0) { errno = (int)(-r); return NULL; }
    DIR *d = (DIR *)malloc(sizeof(DIR));
    if (!d) { suki_syscall1(SYS_CLOSEDIR, (uint64_t)r); errno = ENOMEM; return NULL; }
    d->fd  = (int)r;
    d->pos = 0;
    d->eof = 0;
    memset(&d->ent, 0, sizeof(d->ent));
    return d;
}

struct dirent *readdir(DIR *dirp)
{
    if (!dirp || dirp->fd < 0) { errno = EBADF; return NULL; }
    if (dirp->eof) return NULL;
    /* 直接把内核 suki_dirent_t 读进 libc dirent（布局一致）。 */
    long r = suki_syscall2(SYS_READDIR, (uint64_t)dirp->fd,
                                          (uint64_t)&dirp->ent);
    if (r == 0) {                 /* 目录末尾 */
        dirp->eof = 1;
        return NULL;
    }
    if (r < 0) { errno = (int)(-r); return NULL; }
    /* 内核返回 d_off（下一条记录偏移），同步到 telldir 游标。 */
    dirp->pos = (long)dirp->ent.d_off;
    return &dirp->ent;
}

int closedir(DIR *dirp)
{
    if (!dirp) { errno = EINVAL; return -1; }
    if (dirp->fd >= 0) {
        long r = suki_syscall1(SYS_CLOSEDIR, (uint64_t)dirp->fd);
        dirp->fd = -1;
        if (r < 0) { free(dirp); errno = (int)(-r); return -1; }
    }
    free(dirp);
    return 0;
}

long telldir(DIR *dirp)
{
    if (!dirp || dirp->fd < 0) { errno = EBADF; return -1; }
    long r = suki_syscall1(SYS_TELLDIR, (uint64_t)dirp->fd);
    if (r < 0) { errno = (int)(-r); return -1; }
    return r;
}

void seekdir(DIR *dirp, long loc)
{
    if (!dirp || dirp->fd < 0) { errno = EBADF; return; }
    long r = suki_syscall2(SYS_SEEKDIR, (uint64_t)dirp->fd, (uint64_t)loc);
    if (r < 0) { errno = (int)(-r); return; }
    dirp->pos = loc;
    dirp->eof = 0;   /* 重新定位后清掉 EOF 标记 */
}

void rewinddir(DIR *dirp)
{
    if (!dirp) { errno = EINVAL; return; }
    seekdir(dirp, 0);
}
