/*
 * user/lib/shims/dirent.h
 * -----------------------------------------------------------------------------
 * SukiOS 用户态标准 <dirent.h> 实现头。
 *
 * 实现归属：user/lib/dirent.c（opendir/readdir/closedir/telldir/seekdir/rewinddir），
 * 后端为内核 SYS_OPENDIR/READDIR/CLOSEDIR/TELLDIR/SEEKDIR。struct dirent 布局与
 * 内核 suki_dirent_t 一致（user/lib/libc.h 中定义），此处 reuse 同一结构。
 */
#ifndef _SUKI_SHIM_DIRENT_H
#define _SUKI_SHIM_DIRENT_H

#include <stddef.h>
#include <stdint.h>

/* 目录项结构（与内核 IPC 返回的 suki_dirent_t 布局一致） */
struct dirent {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    uint8_t  _pad[5];
    char     d_name[256];
};

/* d_type 取值（与 POSIX <dirent.h> 一致） */
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12

/* 把 d_type 当成目录判断 */
#define DT_ISDIR(t) ((t) == DT_DIR)

/* 目录流句柄（不透明，后端为内核 opendir fd） */
typedef struct DIR DIR;

/* ---- 目录遍历接口（user/lib/dirent.c） ---- */
DIR           *opendir(const char *name);
struct dirent *readdir(DIR *dirp);
int            closedir(DIR *dirp);
long           telldir(DIR *dirp);
void           seekdir(DIR *dirp, long loc);
void           rewinddir(DIR *dirp);

#endif /* _SUKI_SHIM_DIRENT_H */
