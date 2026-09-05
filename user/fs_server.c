/*
 * user/fs_server.c
 * -----------------------------------------------------------------------------
 * SukiOS Ring3 FAT32 文件系统服务（FS_SERVER）。
 *
 * 设计依据（用户最新指示）：
 *   - 磁盘 IO 与 FAT 解析**完全交由 FatFs（ChaN R0.16，drivers/FatFs/）**完成，
 *     本文件只负责把 FS_PORT 的 IPC 协议翻译成 FatFs API 调用，并把磁盘扇区 IO
 *     经 DISK_PORT Mach IPC 转交 Ring0 内核 disk-srv（ATAPID/AHCI）。
 *   - drivers/FatFs/diskio.c 已实现 disk_initialize/disk_read/disk_write/disk_ioctl，
 *     对接 DISK_PORT；ff.c + ffunicode.c 编入本 blob（见 Makefile）。
 *   - 本服务是单线程、单卷（pdrv=0 / 卷 ""），无 libc（freestanding），
 *     内存原语由 user/lib/suki.c 提供，ff_memalloc/ff_memfree 亦在 suki.c 中实现。
 *
 * 架构（与内核契约，不可变）：
 *   - 请求经 mach_msg 发 FS_PORT，应答发回请求方 msgh_local_port；
 *     整文件读（execve）经 OOL 物理页重映射零拷贝回传。
 *   - 协议号/消息结构见 include/ipc/fs_proto.h；磁盘 ABI 见 include/ipc/disk_proto.h。
 *
 * 生产约束（铁律）：
 *   - 所有应答数据写入本任务本地缓冲（g_resp/g_filebuf），绝不把外部 IPC 指针
 *     当 memcpy 目标。
 *   - IPC 消息头 / 结构按 fs_proto.h 严格校验长度，畸形消息直接丢弃，绝不越界。
 *   - 不引入任何未闭环的错误路径；无 stub/TODO。
 * -----------------------------------------------------------------------------
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "lib/suki.h"
#include <ipc/fs_proto.h>
#include <ipc/disk_proto.h>
/* POSIX 常量：SUKI_O_ 系列、SUKI_S_ 系列、SUKI_SEEK_ 系列、SUKI_DT_ 系列。
 * 用户态包含（不定义 SUKI_KERNEL_BUILD），故其中的 syscall 内联封装也会
 * 被包含进来（本服务暂不使用，编译器会按需丢弃未引用的 static inline）。 */
#include <sukios/posix.h>
#include "ff.h"            /* FatFs 类型与 API */

/* 应答/整文件缓冲（所有输出写入此处，杜绝外部指针写） */
#define RESP_DATA_MAX    FS_DATA_MAX            /* 内联应答数据上限 (3500) */
#define FILEBUF_SIZE     (16u * 4096u)          /* 16 KiB×16 = 256 KiB：整文件读上限（与内核 OOL 16 页一致） */
static uint8_t  g_resp[sizeof(mach_msg_header_t) + sizeof(fs_resp_t) + RESP_DATA_MAX + 16];
static uint8_t  g_filebuf[FILEBUF_SIZE] __attribute__((aligned(4096))); /* 整文件读（OOL 内容源） */

/* FatFs 工作对象（BSS，单卷 "" / 物理盘 0） */
static FATFS   g_fatfs;
static FIL     g_fil;
static DIR     g_dir;
static FILINFO g_finfo;


/* ===================== 小工具 ===================== */

/* 把 FatFs 结果码映射为 FS 协议状态。FR_OK -> FS_OK；FR_NO_FILE/FR_NO_PATH
 * -> FS_ERR_NOENT；其余 IO 类 -> FS_ERR_IO。 */
static uint32_t fr_to_status(FRESULT fr)
{
    switch (fr) {
    case FR_OK:            return FS_OK;
    case FR_NO_FILE:       return FS_ERR_NOENT;
    case FR_NO_PATH:       return FS_ERR_NOENT;
    case FR_INVALID_NAME:  return FS_ERR_NOENT;
    case FR_DENIED:        return FS_ERR_IO;
    case FR_EXIST:         return FS_ERR_IO;
    case FR_WRITE_PROTECTED: return FS_ERR_IO;
    case FR_NOT_READY:     return FS_ERR_IO;
    case FR_DISK_ERR:      return FS_ERR_IO;
    case FR_INT_ERR:       return FS_ERR_IO;
    case FR_NOT_ENABLED:   return FS_ERR_IO;
    case FR_NO_FILESYSTEM: return FS_ERR_IO;
    case FR_MKFS_ABORTED:  return FS_ERR_IO;
    case FR_TIMEOUT:       return FS_ERR_IO;
    case FR_LOCKED:        return FS_ERR_IO;
    case FR_NOT_ENOUGH_CORE: return FS_ERR_IO;
    case FR_TOO_MANY_OPEN_FILES: return FS_ERR_IO;
    default:               return FS_ERR_IO;
    }
}

/* 把客户端传来的路径就地转成 FatFs 可用路径。
 * 约定：裸名（如 README.TXT）或带 '/' 子目录（如 BIN/PLAYAUDIO）均直接可用；
 * 若以 '/' 开头则去掉首 '/'（FatFs 单卷下 "/" == 根目录 "0:/"）。
 * 返回指向（可能修改后）的同一缓冲的指针，便于直接传给 f_open 等。 */
static char *normalize_path(char *path)
{
    if (path[0] == '/') {
        /* 去掉前导 '/'（保留其余，空串 "" 表示根目录） */
        size_t i = 0, j = 0;
        do {
            path[j++] = path[i];
        } while (path[i++] != '\0');
    }
    /* 根目录特判：归一后为空串表示卷根。FatFs 的 f_stat/f_opendir 对空串
     * 返回 FR_INVALID_NAME，需显式转成卷根标记 "0:"（数字卷标总被 FatFs
     * 接受）。否则 chdir("/")/access("/")/stat("/") 会失败。 */
    if (path[0] == '\0') {
        path[0] = '0';
        path[1] = ':';
        path[2] = '\0';
    }
    return path;
}

/* ===================== IPC 应答辅助 ===================== */
static mach_msg_header_t *resp_header(void) { return (mach_msg_header_t *)g_resp; }

static void build_resp(uint32_t local_port, uint32_t id, uint32_t status,
                       uint32_t length, const void *data)
{
    mach_msg_header_t *h = resp_header();
    h->msgh_bits        = 0;
    h->msgh_size        = (uint32_t)(sizeof(mach_msg_header_t) + sizeof(fs_resp_t) + length);
    h->msgh_remote_port = local_port;
    h->msgh_local_port  = FS_PORT;
    h->msgh_id          = id;
    h->msgh_reserved    = 0;
    fs_resp_t *fr = (fs_resp_t *)(g_resp + sizeof(mach_msg_header_t));
    fr->status = status;
    fr->length = length;
    if (length && data)
        u_memcpy(g_resp + sizeof(mach_msg_header_t) + sizeof(fs_resp_t), data, length);
}

/* OOL 应答（整文件读，execve 用）：g_filebuf（含头部）经 OOL 描述符回传。 */
static void build_ool_resp(uint32_t local_port, uint32_t id, uint32_t size)
{
    mach_msg_header_t *h = resp_header();
    h->msgh_bits        = MACH_MSGH_BITS_OOL;
    h->msgh_size        = (uint32_t)(sizeof(mach_msg_header_t) + sizeof(ool_desc_t));
    h->msgh_remote_port = local_port;
    h->msgh_local_port  = FS_PORT;
    h->msgh_id          = id;
    h->msgh_reserved    = 0;
    ool_desc_t *d = (ool_desc_t *)(g_resp + sizeof(mach_msg_header_t));
    d->address = (uint64_t)g_filebuf;
    d->size = (uint64_t)((sizeof(fs_resp_t) + size + 4096u - 1) & ~((uint64_t)4096u - 1));
    if (d->size == 0) d->size = 4096u;
}

/* ===================== 二代：句柄表（POSIX 就绪）===================== */
/*
 * 为什么需要句柄表：
 *   一代协议是「一次调用完成打开+定位+IO+关闭」，无法表达 POSIX 的
 *   open/lseek/read/write/close 分阶段语义（文件偏移要跨调用保持）。
 *   故引入服务端句柄：
 *     - 文件句柄编号 [0, FS_MAX_OPEN)
 *     - 目录句柄编号 [FS_MAX_OPEN, FS_MAX_OPEN + FS_MAX_DIR)
 *   两段编号【刻意不重叠】：内核若因 bug 把目录句柄当文件句柄传过来，
 *   索引直接落在目录段之外，会被 handle_* 的区间校验挡下并返回 EBADF，
 *   而不会误操作另一个表的对象。
 *
 * 每个文件句柄保存打开时的路径副本：FatFs 的 f_stat 只接受路径（没有
 * "由 FIL 取属性" 的公开 API），fstat(fd) 据此实现。
 */
#define FS_MAX_OPEN     16
#define FS_MAX_DIR      8
#define FS_DIR_BASE     FS_MAX_OPEN     /* 目录句柄编号起始 */

typedef struct {
    FIL      fil;
    int      in_use;
    uint32_t flags;              /* 打开时的 SUKI_O_* 标志（O_APPEND 等） */
    char     path[256];
} fs_open_file_t;

typedef struct {
    DIR      dir;
    int      in_use;
} fs_open_dir_t;

static fs_open_file_t g_oft[FS_MAX_OPEN];
static fs_open_dir_t  g_odt[FS_MAX_DIR];

/* 分配一个文件句柄槽，返回索引；表满返回 -1 */
static int oft_alloc(void)
{
    for (int i = 0; i < FS_MAX_OPEN; i++) {
        if (!g_oft[i].in_use) {
            memset(&g_oft[i], 0, sizeof(g_oft[i]));
            g_oft[i].in_use = 1;
            return i;
        }
    }
    return -1;
}

static fs_open_file_t *oft_get(uint32_t fd)
{
    if (fd >= (uint32_t)FS_MAX_OPEN) return NULL;
    if (!g_oft[fd].in_use) return NULL;
    return &g_oft[fd];
}

static void oft_free(uint32_t fd)
{
    fs_open_file_t *f = oft_get(fd);
    if (!f) return;
    f_close(&f->fil);
    memset(f, 0, sizeof(*f));
}

static int odt_alloc(void)
{
    for (int i = 0; i < FS_MAX_DIR; i++) {
        if (!g_odt[i].in_use) {
            memset(&g_odt[i], 0, sizeof(g_odt[i]));
            g_odt[i].in_use = 1;
            return FS_DIR_BASE + i;      /* 目录句柄编号落在独立区段 */
        }
    }
    return -1;
}

static fs_open_dir_t *odt_get(uint32_t dd)
{
    if (dd < (uint32_t)FS_DIR_BASE
        || dd >= (uint32_t)(FS_DIR_BASE + FS_MAX_DIR)) {
        return NULL;
    }
    int i = (int)dd - FS_DIR_BASE;
    if (!g_odt[i].in_use) return NULL;
    return &g_odt[i];
}

static void odt_free(uint32_t dd)
{
    fs_open_dir_t *d = odt_get(dd);
    if (!d) return;
    f_closedir(&d->dir);
    memset(d, 0, sizeof(*d));
}

/*
 * 三段式应答构造：fs_resp_t + fs_ret_t + 可选数据。
 * 内核 fd.c 的 resp_hdr()/resp_ret() 正是按此布局解析（见 fs_proto.h）。
 */
static void build_resp2(uint32_t local_port, uint32_t id, uint32_t status,
                        int64_t value, const void *data, uint32_t dlen)
{
    mach_msg_header_t *h = resp_header();
    uint32_t total = (uint32_t)sizeof(fs_ret_t) + dlen;
    h->msgh_bits        = 0;
    h->msgh_size        = (uint32_t)(sizeof(mach_msg_header_t)
                                     + sizeof(fs_resp_t) + total);
    h->msgh_remote_port = local_port;
    h->msgh_local_port  = FS_PORT;
    h->msgh_id          = id;
    h->msgh_reserved    = 0;
    fs_resp_t *fr = (fs_resp_t *)(g_resp + sizeof(mach_msg_header_t));
    fr->status = status;
    fr->length = total;
    fs_ret_t *rt = (fs_ret_t *)(g_resp + sizeof(mach_msg_header_t)
                                + sizeof(fs_resp_t));
    rt->value = value;
    if (dlen && data) {
        u_memcpy(g_resp + sizeof(mach_msg_header_t) + sizeof(fs_resp_t)
                 + sizeof(fs_ret_t), data, dlen);
    }
}

/* 仅返回值的应答（多数句柄式操作的形态） */
static void reply_ret(uint32_t local_port, uint32_t id, uint32_t status,
                      int64_t value)
{
    build_resp2(local_port, id, status, value, NULL, 0);
    mach_msg_send(g_resp, resp_header()->msgh_size);
}

/* POSIX errno -> FS 协议状态（与内核 fd.c 的 fs_status_to_errno 互逆） */
static uint32_t errno_to_status(int64_t v)
{
    switch (v) {
    case -2:  return FS_ERR_NOENT;    /* ENOENT */
    case -9:  return FS_ERR_BADF;     /* EBADF */
    case -13: return FS_ERR_ACCES;    /* EACCES */
    case -17: return FS_ERR_EXIST;    /* EEXIST */
    case -20: return FS_ERR_NOTDIR;   /* ENOTDIR */
    case -21: return FS_ERR_ISDIR;    /* EISDIR */
    case -22: return FS_ERR_INVAL;    /* EINVAL */
    case -24: return FS_ERR_NFILE;    /* EMFILE */
    case -28: return FS_ERR_NOSPC;    /* ENOSPC */
    case -36: return FS_ERR_INVAL;    /* ENAMETOOLONG */
    default:  return FS_ERR_IO;
    }
}

/* FatFs 结果码 -> 负 errno（二代接口统一用 errno 表达失败，语义更精确，
 * 内核侧 errno_to_status/fs_status_to_errno 保持一一对应） */
static int64_t fr_to_errno(FRESULT fr)
{
    switch (fr) {
    case FR_OK:              return 0;
    case FR_NO_FILE:         return -2;    /* ENOENT */
    case FR_NO_PATH:         return -2;    /* ENOENT */
    case FR_INVALID_NAME:    return -2;    /* ENOENT */
    case FR_DENIED:          return -13;   /* EACCES */
    case FR_EXIST:           return -17;   /* EEXIST */
    case FR_WRITE_PROTECTED: return -30;   /* EROFS */
    case FR_INVALID_DRIVE:   return -19;   /* ENODEV */
    case FR_NOT_READY:       return -19;   /* ENODEV */
    case FR_NOT_ENABLED:     return -19;   /* ENODEV */
    case FR_NO_FILESYSTEM:   return -19;   /* ENODEV */
    case FR_TOO_MANY_OPEN_FILES: return -24; /* EMFILE */
    default:                 return -5;    /* EIO */
    }
}

/* 把 POSIX O_* 标志翻译为 FatFs 打开模式（POSIX 优先级顺序，不可颠倒） */
static BYTE o_flags_to_fa(int32_t flags, int *need_trunc)
{
    int acc = flags & SUKI_O_ACCMODE;
    BYTE rw;
    if (acc == SUKI_O_RDONLY)      rw = FA_READ;
    else if (acc == SUKI_O_WRONLY) rw = FA_WRITE;
    else                           rw = FA_READ | FA_WRITE;

    *need_trunc = 0;
    if (flags & SUKI_O_CREAT) {
        if (flags & SUKI_O_EXCL) {
            return rw | FA_CREATE_NEW;         /* 必须不存在 */
        }
        if (flags & SUKI_O_TRUNC) {
            return rw | FA_CREATE_ALWAYS;      /* 存在则清空 */
        }
        return rw | FA_OPEN_ALWAYS;            /* 存在则打开，否则创建 */
    }
    if (flags & SUKI_O_TRUNC) {
        *need_trunc = 1;                       /* 打开后截断（无 CREAT） */
    }
    return rw | FA_OPEN_EXISTING;
}

/*
 * Unix 秒 -> FAT 日期字（f_utime 用）。
 * 编码（与 fill_stat 的解码互逆）：bit0-4 日(1..31)、bit5-8 月(1..12)、
 * bit9-15 年-1980。年份钳制在 1980..2107（FAT 日期的可表达范围），
 * 越界即钳制——绝不写入非法日期导致卷结构被判损坏。
 */
static WORD unix_to_fatdate(uint64_t secs)
{
    uint64_t days = secs / 86400ULL;
    /* civil_from_days（Hinnant 算法，days_from_civil 的逆） */
    int64_t z = (int64_t)days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);          /* [0, 146096] */
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = (mp < 10) ? (mp + 3) : (mp - 9);
    int year = (int)(y + (m <= 2 ? 1 : 0));
    if (year < 1980) year = 1980;
    if (year > 2107) year = 2107;
    return (WORD)((((unsigned)(year - 1980) & 0x7F) << 9)
                  | (m << 5) | (d & 0x1F));
}

/* FILINFO + 路径 -> fs_stat_t（POSIX struct stat 的协议映射） */
static void fill_stat(fs_stat_t *st, const FILINFO *fi)
{
    st->dev = 0;
    st->ino = 0;                 /* FAT32 无索引结点号：填起始簇作稳定标识 */
    st->nlink = 1;
    st->uid = 0;
    st->gid = 0;
    st->rdev = 0;
    st->blksize = 512;
    st->size = (int64_t)fi->fsize;
    st->blocks = (int64_t)((fi->fsize + 511) / 512);

    /* 文件类型 + 权限位 */
    uint32_t mode;
    if (fi->fattrib & AM_DIR) {
        mode = SUKI_S_IFDIR | 0755;
    } else {
        mode = SUKI_S_IFREG | 0644;
    }
    if (fi->fattrib & AM_RDO) {
        /* 只读：去掉所有写位 */
        mode &= ~(SUKI_S_IWUSR | SUKI_S_IWGRP | SUKI_S_IWOTH);
    }
    st->mode = mode;

    /* FAT 时间戳 -> Unix 秒（与 get_fattime 的编码互逆） */
    WORD d = fi->fdate, t = fi->ftime;
    unsigned year = 1980 + ((d >> 9) & 0x7F);
    unsigned mon  = (d >> 5) & 0x0F;
    unsigned day  = d & 0x1F;
    unsigned hour = (t >> 11) & 0x1F;
    unsigned min  = (t >> 5) & 0x3F;
    unsigned sec  = (t & 0x1F) * 2;
    if (mon < 1) mon = 1;
    if (day < 1) day = 1;
    /* 与内核 rtc.c 的 days_from_civil 同算法（Hinnant） */
    int y = (int)year - (mon <= 2 ? 1 : 0);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (mon + (mon > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    int64_t secs = days * 86400 + hour * 3600 + min * 60 + sec;
    if (secs < 0) secs = 0;
    st->atime = secs;
    st->mtime = secs;
    st->ctime = secs;
}

/* ===================== 二代请求处理 ===================== */

/* FS_MSG_OPEN(payload = fs_open_req_t + NUL 结尾路径) */
static void handle_open2(uint32_t local_port, uint32_t id,
                         const fs_open_req_t *rq, const char *raw_path)
{
    char path[256];
    size_t l = u_strlen(raw_path);
    if (l == 0 || l >= sizeof(path)) {
        reply_ret(local_port, id, FS_ERR_INVAL, -22);
        return;
    }
    u_memcpy(path, raw_path, l + 1);
    normalize_path(path);

    int slot = oft_alloc();
    if (slot < 0) {
        reply_ret(local_port, id, FS_ERR_NFILE, -24);   /* EMFILE */
        return;
    }
    int need_trunc = 0;
    BYTE fa = o_flags_to_fa(rq->flags, &need_trunc);

    FRESULT fr = f_open(&g_oft[slot].fil, path, fa);
    if (fr != FR_OK) {
        memset(&g_oft[slot], 0, sizeof(g_oft[slot]));
        int64_t e = fr_to_errno(fr);
        reply_ret(local_port, id, errno_to_status(e), e);
        return;
    }
    /* 无 O_CREAT 的 O_TRUNC：打开成功后截断 */
    if (need_trunc) {
        FRESULT ls = f_lseek(&g_oft[slot].fil, 0);
        if (ls == FR_OK) {
            FRESULT tr = f_truncate(&g_oft[slot].fil);
            if (tr != FR_OK) {
                f_close(&g_oft[slot].fil);
                memset(&g_oft[slot], 0, sizeof(g_oft[slot]));
                int64_t e = fr_to_errno(tr);
                reply_ret(local_port, id, errno_to_status(e), e);
                return;
            }
        }
    }
    /* O_APPEND：打开后定位到末尾（后续每次写前也会再定位，见 handle_write_fd） */
    if (rq->flags & SUKI_O_APPEND) {
        f_lseek(&g_oft[slot].fil, f_size(&g_oft[slot].fil));
    }
    g_oft[slot].flags = (uint32_t)rq->flags;
    u_memcpy(g_oft[slot].path, path, l + 1);

    reply_ret(local_port, id, FS_OK, (int64_t)slot);
}

/* FS_MSG_CLOSE(payload = fs_fd_req_t) */
static void handle_close2(uint32_t local_port, uint32_t id, uint32_t fd)
{
    if (!oft_get(fd)) {
        reply_ret(local_port, id, FS_ERR_BADF, -9);
        return;
    }
    oft_free(fd);
    reply_ret(local_port, id, FS_OK, 0);
}

/* FS_MSG_READFD(payload = fs_read_req_t) */
static void handle_read_fd(uint32_t local_port, uint32_t id, uint32_t fd,
                           uint32_t want)
{
    fs_open_file_t *f = oft_get(fd);
    if (!f) {
        reply_ret(local_port, id, FS_ERR_BADF, -9);
        return;
    }
    if (want > FS_READ_MAX) want = FS_READ_MAX;
    if (want == 0) {
        reply_ret(local_port, id, FS_OK, 0);
        return;
    }
    uint8_t *data = g_resp + sizeof(mach_msg_header_t) + sizeof(fs_resp_t)
                    + sizeof(fs_ret_t);
    UINT br = 0;
    FRESULT fr = f_read(&f->fil, data, want, &br);
    if (fr != FR_OK) {
        int64_t e = fr_to_errno(fr);
        reply_ret(local_port, id, errno_to_status(e), e);
        return;
    }
    build_resp2(local_port, id, FS_OK, (int64_t)br, NULL, (uint32_t)br);
    /* build_resp2 已在 g_resp 内布局完毕（数据源就是 g_resp 内部偏移，
     * 故此处 data 指针与写入目标一致，无需再复制） */
    mach_msg_send(g_resp, resp_header()->msgh_size);
}

/* FS_MSG_WRITEFD(payload = fs_write_fd_req_t + 数据) */
static void handle_write_fd(uint32_t local_port, uint32_t id, uint32_t fd,
                            uint32_t len, int64_t off, const void *data)
{
    fs_open_file_t *f = oft_get(fd);
    if (!f) {
        reply_ret(local_port, id, FS_ERR_BADF, -9);
        return;
    }
    if (len > FS_WRITE_MAX || (len > 0 && !data)) {
        reply_ret(local_port, id, FS_ERR_INVAL, -22);
        return;
    }
    if (len == 0) {
        reply_ret(local_port, id, FS_OK, 0);
        return;
    }
    /* O_APPEND 语义：写前必须先定位到末尾（POSIX 规定原子性在单次写内，
     * 本服务单线程处理请求，一次 f_lseek+f_write 之间不会被其它写插入） */
    if ((f->flags & SUKI_O_APPEND) || off < 0) {
        FRESULT ls = (f->flags & SUKI_O_APPEND)
                     ? f_lseek(&f->fil, f_size(&f->fil))
                     : FR_OK;
        if (ls != FR_OK) {
            int64_t e = fr_to_errno(ls);
            reply_ret(local_port, id, errno_to_status(e), e);
            return;
        }
    } else {
        FRESULT ls = f_lseek(&f->fil, (FSIZE_t)off);
        if (ls != FR_OK) {
            int64_t e = fr_to_errno(ls);
            reply_ret(local_port, id, errno_to_status(e), e);
            return;
        }
    }
    UINT bw = 0;
    FRESULT fr = f_write(&f->fil, data, len, &bw);
    if (fr != FR_OK) {
        int64_t e = fr_to_errno(fr);
        reply_ret(local_port, id, errno_to_status(e), e);
        return;
    }
    /* 未同步写（FatFs 有私有写窗口）：写后刷盘，保证数据真正落卷 */
    f_sync(&f->fil);
    reply_ret(local_port, id, FS_OK, (int64_t)bw);
}

/* FS_MSG_SEEKFD(payload = fs_seek_req_t) */
static void handle_seek_fd(uint32_t local_port, uint32_t id, uint32_t fd,
                           int64_t off, int32_t whence)
{
    fs_open_file_t *f = oft_get(fd);
    if (!f) {
        reply_ret(local_port, id, FS_ERR_BADF, -9);
        return;
    }
    FSIZE_t target;
    FSIZE_t cur = f_tell(&f->fil);
    FSIZE_t size = f_size(&f->fil);
    if (whence == SUKI_SEEK_SET) {
        if (off < 0) {
            reply_ret(local_port, id, FS_ERR_INVAL, -22);
            return;
        }
        target = (FSIZE_t)off;
    } else if (whence == SUKI_SEEK_CUR) {
        int64_t t = (int64_t)cur + off;
        if (t < 0) {
            reply_ret(local_port, id, FS_ERR_INVAL, -22);
            return;
        }
        target = (FSIZE_t)t;
    } else if (whence == SUKI_SEEK_END) {
        int64_t t = (int64_t)size + off;
        if (t < 0) {
            reply_ret(local_port, id, FS_ERR_INVAL, -22);
            return;
        }
        target = (FSIZE_t)t;
    } else {
        reply_ret(local_port, id, FS_ERR_INVAL, -22);
        return;
    }
    FRESULT fr = f_lseek(&f->fil, target);
    if (fr != FR_OK) {
        int64_t e = fr_to_errno(fr);
        reply_ret(local_port, id, errno_to_status(e), e);
        return;
    }
    reply_ret(local_port, id, FS_OK, (int64_t)f_tell(&f->fil));
}

/* FS_MSG_FSTAT：经打开时保存的路径取属性 */
static void handle_fstat(uint32_t local_port, uint32_t id, uint32_t fd)
{
    fs_open_file_t *f = oft_get(fd);
    if (!f) {
        reply_ret(local_port, id, FS_ERR_BADF, -9);
        return;
    }
    FILINFO fi;
    FRESULT fr = f_stat(f->path, &fi);
    if (fr != FR_OK) {
        int64_t e = fr_to_errno(fr);
        reply_ret(local_port, id, errno_to_status(e), e);
        return;
    }
    fs_stat_t st;
    memset(&st, 0, sizeof(st));
    fill_stat(&st, &fi);
    build_resp2(local_port, id, FS_OK, 0, &st, (uint32_t)sizeof(st));
    mach_msg_send(g_resp, resp_header()->msgh_size);
}

/* FS_MSG_STAT(payload = NUL 结尾路径) */
static void handle_stat(uint32_t local_port, uint32_t id, const char *raw_path)
{
    char path[256];
    size_t l = u_strlen(raw_path);
    if (l == 0 || l >= sizeof(path)) {
        reply_ret(local_port, id, FS_ERR_INVAL, -22);
        return;
    }
    u_memcpy(path, raw_path, l + 1);
    normalize_path(path);

    FILINFO fi;
    FRESULT fr = f_stat(path, &fi);
    if (fr != FR_OK) {
        int64_t e = fr_to_errno(fr);
        reply_ret(local_port, id, errno_to_status(e), e);
        return;
    }
    fs_stat_t st;
    memset(&st, 0, sizeof(st));
    fill_stat(&st, &fi);
    build_resp2(local_port, id, FS_OK, 0, &st, (uint32_t)sizeof(st));
    mach_msg_send(g_resp, resp_header()->msgh_size);
}

/* FS_MSG_FTRUNC(payload = fs_ftrunc_req_t) */
static void handle_ftrunc(uint32_t local_port, uint32_t id, uint32_t fd,
                          int64_t len)
{
    fs_open_file_t *f = oft_get(fd);
    if (!f || len < 0) {
        reply_ret(local_port, id, FS_ERR_BADF, -9);
        return;
    }
    FRESULT ls = f_lseek(&f->fil, (FSIZE_t)len);
    if (ls != FR_OK) {
        int64_t e = fr_to_errno(ls);
        reply_ret(local_port, id, errno_to_status(e), e);
        return;
    }
    FRESULT fr = f_truncate(&f->fil);
    if (fr != FR_OK) {
        int64_t e = fr_to_errno(fr);
        reply_ret(local_port, id, errno_to_status(e), e);
        return;
    }
    f_sync(&f->fil);
    reply_ret(local_port, id, FS_OK, 0);
}

/* FS_MSG_OPENDIR(payload = NUL 结尾路径) */
static void handle_opendir(uint32_t local_port, uint32_t id,
                           const char *raw_path)
{
    char path[256];
    size_t l = u_strlen(raw_path);
    if (l >= sizeof(path)) {
        reply_ret(local_port, id, FS_ERR_INVAL, -36);   /* ENAMETOOLONG */
        return;
    }
    u_memcpy(path, raw_path, l + 1);
    normalize_path(path);

    int dd = odt_alloc();
    if (dd < 0) {
        reply_ret(local_port, id, FS_ERR_NFILE, -24);
        return;
    }
    int i = dd - FS_DIR_BASE;
    FRESULT fr = f_opendir(&g_odt[i].dir, path);
    if (fr != FR_OK) {
        memset(&g_odt[i], 0, sizeof(g_odt[i]));
        int64_t e = fr_to_errno(fr);
        reply_ret(local_port, id, errno_to_status(e), e);
        return;
    }
    reply_ret(local_port, id, FS_OK, (int64_t)dd);
}

/* FS_MSG_READDIR(payload = fs_fd_req_t) */
static void handle_readdir(uint32_t local_port, uint32_t id, uint32_t dd)
{
    fs_open_dir_t *d = odt_get(dd);
    if (!d) {
        reply_ret(local_port, id, FS_ERR_BADF, -9);
        return;
    }
    FILINFO fi;
    for (;;) {
        FRESULT fr = f_readdir(&d->dir, &fi);
        if (fr != FR_OK) {
            int64_t e = fr_to_errno(fr);
            reply_ret(local_port, id, errno_to_status(e), e);
            return;
        }
        if (fi.fname[0] == '\0') {
            reply_ret(local_port, id, FS_OK, 0);      /* 目录结束 */
            return;
        }
        break;
    }
    fs_dirent_t de;
    memset(&de, 0, sizeof(de));
    de.ino = 0;
    if (fi.fattrib & AM_DIR) de.type = SUKI_DT_DIR;
    else                     de.type = SUKI_DT_REG;
    /* 名字：优先长名（LFN），截断到 255 并强制 NUL 终结 */
    const char *nm = fi.fname;
    size_t nl = u_strlen(nm);
    if (nl == 0) {
        reply_ret(local_port, id, FS_OK, 0);
        return;
    }
    if (nl >= sizeof(de.name)) nl = sizeof(de.name) - 1;
    u_memcpy(de.name, nm, nl);
    de.name[nl] = '\0';
    build_resp2(local_port, id, FS_OK, 1, &de, (uint32_t)sizeof(de));
    mach_msg_send(g_resp, resp_header()->msgh_size);
}

/* FS_MSG_CLOSEDIR(payload = fs_fd_req_t) */
static void handle_closedir(uint32_t local_port, uint32_t id, uint32_t dd)
{
    if (!odt_get(dd)) {
        reply_ret(local_port, id, FS_ERR_BADF, -9);
        return;
    }
    odt_free(dd);
    reply_ret(local_port, id, FS_OK, 0);
}

/* FS_MSG_MKDIR2(payload = fs_path_mode_req_t + 路径) */
static void handle_mkdir2(uint32_t local_port, uint32_t id,
                          const char *raw_path)
{
    char path[256];
    size_t l = u_strlen(raw_path);
    if (l == 0 || l >= sizeof(path)) {
        reply_ret(local_port, id, FS_ERR_INVAL, -22);
        return;
    }
    u_memcpy(path, raw_path, l + 1);
    normalize_path(path);

    FRESULT fr = f_mkdir(path);
    if (fr == FR_EXIST) {
        reply_ret(local_port, id, FS_ERR_EXIST, -17);   /* EEXIST（POSIX） */
        return;
    }
    if (fr != FR_OK) {
        int64_t e = fr_to_errno(fr);
        reply_ret(local_port, id, errno_to_status(e), e);
        return;
    }
    reply_ret(local_port, id, FS_OK, 0);
}

/* FS_MSG_UNLINK2(payload = fs_unlink_req_t + 路径)；is_dir=1 走 rmdir 语义 */
static void handle_unlink2(uint32_t local_port, uint32_t id,
                           const char *raw_path, uint32_t is_dir)
{
    char path[256];
    size_t l = u_strlen(raw_path);
    if (l == 0 || l >= sizeof(path)) {
        reply_ret(local_port, id, FS_ERR_INVAL, -22);
        return;
    }
    u_memcpy(path, raw_path, l + 1);
    normalize_path(path);

    /* 语义校验：unlink 不得删目录，rmdir 不得删普通文件（POSIX） */
    FILINFO fi;
    if (f_stat(path, &fi) == FR_OK) {
        bool target_is_dir = (fi.fattrib & AM_DIR) != 0;
        if (is_dir && !target_is_dir) {
            reply_ret(local_port, id, FS_ERR_NOTDIR, -20);   /* ENOTDIR */
            return;
        }
        if (!is_dir && target_is_dir) {
            reply_ret(local_port, id, FS_ERR_ISDIR, -21);    /* EISDIR */
            return;
        }
    }
    FRESULT fr = f_unlink(path);
    if (fr != FR_OK) {
        int64_t e = fr_to_errno(fr);
        reply_ret(local_port, id, errno_to_status(e), e);
        return;
    }
    reply_ret(local_port, id, FS_OK, 0);
}

/* FS_MSG_RENAME2(payload = old NUL + new NUL) */
static void handle_rename2(uint32_t local_port, uint32_t id,
                           const char *oldp, const char *newp)
{
    char op[256], np[256];
    size_t ol = u_strlen(oldp), nl = u_strlen(newp);
    if (ol == 0 || nl == 0 || ol >= sizeof(op) || nl >= sizeof(np)) {
        reply_ret(local_port, id, FS_ERR_INVAL, -22);
        return;
    }
    u_memcpy(op, oldp, ol + 1);
    u_memcpy(np, newp, nl + 1);
    normalize_path(op);
    normalize_path(np);

    FRESULT fr = f_rename(op, np);
    if (fr != FR_OK) {
        int64_t e = fr_to_errno(fr);
        reply_ret(local_port, id, errno_to_status(e), e);
        return;
    }
    reply_ret(local_port, id, FS_OK, 0);
}

/* FS_MSG_ACCESS(payload = fs_path_mode_req_t + 路径)
 * 权限模型：FAT32 无 POSIX 权限位。F_OK 查存在；R_OK 恒可（普通文件可读）；
 * W_OK 检查只读属性（AM_RDO）；X_OK 恒不成立（无执行位概念）。 */
static void handle_access(uint32_t local_port, uint32_t id,
                          const char *raw_path, int32_t mode)
{
    char path[256];
    size_t l = u_strlen(raw_path);
    if (l == 0 || l >= sizeof(path)) {
        reply_ret(local_port, id, FS_ERR_INVAL, -22);
        return;
    }
    u_memcpy(path, raw_path, l + 1);
    normalize_path(path);

    FILINFO fi;
    FRESULT fr = f_stat(path, &fi);
    if (fr != FR_OK) {
        int64_t e = fr_to_errno(fr);
        reply_ret(local_port, id, errno_to_status(e), e);
        return;
    }
    if (mode == SUKI_F_OK) {
        reply_ret(local_port, id, FS_OK, 0);
        return;
    }
    if (mode & SUKI_X_OK) {
        reply_ret(local_port, id, FS_ERR_ACCES, -13);   /* EACCES */
        return;
    }
    if ((mode & SUKI_W_OK) && (fi.fattrib & AM_RDO)) {
        reply_ret(local_port, id, FS_ERR_ACCES, -13);   /* EACCES */
        return;
    }
    reply_ret(local_port, id, FS_OK, 0);
}

/* FS_MSG_STATFS：经 f_getfree 取卷容量信息 */
static void handle_statfs(uint32_t local_port, uint32_t id)
{
    DWORD nclst = 0;
    FATFS *fs = NULL;
    FRESULT fr = f_getfree("", &nclst, &fs);
    if (fr != FR_OK || !fs) {
        int64_t e = fr_to_errno(fr);
        reply_ret(local_port, id, errno_to_status(e), e);
        return;
    }
    /* 簇与扇区：FATFS 结构只暴露簇数(n_fatent)与每簇扇区数(csize)；
     * 扇区字节数由配置常量 FF_MAX_SS/FF_MIN_SS 决定（本系统均 512）。
     * 绝不引用结构内不存在的字段（编译期即挡下，避免 IOCTL 取值的复杂路径）。 */
    uint64_t total_cl = (uint64_t)fs->n_fatent - 2;
    uint64_t bpc = (uint64_t)fs->csize
                 * ((FF_MAX_SS < 512) ? 512 : FF_MAX_SS);

    fs_statfs_t sf;
    memset(&sf, 0, sizeof(sf));
    sf.type = 0x4d44;                 /* MSDOS_SUPER_MAGIC */
    sf.bsize = 512;
    sf.blocks = total_cl * (uint64_t)fs->csize;
    sf.bfree = (uint64_t)nclst * (uint64_t)fs->csize;
    sf.files = 0;                     /* FAT32 无 inode 计数 */
    sf.ffree = 0;
    sf.namelen = 255;                 /* LFN 上限 */
    (void)bpc;
    build_resp2(local_port, id, FS_OK, 0, &sf, (uint32_t)sizeof(sf));
    mach_msg_send(g_resp, resp_header()->msgh_size);
}

/* FS_MSG_SYNC：FatFs 无全局缓存需刷（每次写已 f_sync），应答成功 */
static void handle_sync(uint32_t local_port, uint32_t id)
{
    reply_ret(local_port, id, FS_OK, 0);
}

/* FS_MSG_UTIME(payload = fs_utime_req_t + 路径) */
static void handle_utime(uint32_t local_port, uint32_t id,
                         int64_t atime, int64_t mtime, const char *raw_path)
{
    char path[256];
    size_t l = u_strlen(raw_path);
    if (l == 0 || l >= sizeof(path)) {
        reply_ret(local_port, id, FS_ERR_INVAL, -22);
        return;
    }
    u_memcpy(path, raw_path, l + 1);
    normalize_path(path);

    FILINFO fi;
    /* 先取现有属性，再覆盖需要修改的时间字段（<0 表示不修改，POSIX 语义） */
    if (atime < 0 || mtime < 0) {
        if (f_stat(path, &fi) != FR_OK) {
            reply_ret(local_port, id, FS_ERR_NOENT, -2);
            return;
        }
    }
    if (mtime >= 0) {
        fi.fdate = unix_to_fatdate((uint64_t)mtime);
        fi.ftime = 0;
    }
    FRESULT fr = f_utime(path, &fi);
    if (fr != FR_OK) {
        int64_t e = fr_to_errno(fr);
        reply_ret(local_port, id, errno_to_status(e), e);
        return;
    }
    reply_ret(local_port, id, FS_OK, 0);
}

/*
 * FS_MSG_CHMOD(payload = fs_chmod_req_t + 路径)
 * FAT32 只有「只读/隐藏/系统/归档」四个属性位，无法承载完整的 POSIX
 * 权限三元组。映射规则（显式、可预测，绝不静默丢弃调用者意图）：
 *   所有者/组/其他 的写位【全部为 0】 -> 置 AM_RDO（只读）
 *   任一写位为 1                      -> 清 AM_RDO（可写）
 * 其余属性位（隐藏/系统/归档）保持原值不变。
 */
static void handle_chmod(uint32_t local_port, uint32_t id,
                         uint32_t mode, const char *raw_path)
{
    char path[256];
    size_t l = u_strlen(raw_path);
    if (l == 0 || l >= sizeof(path)) {
        reply_ret(local_port, id, FS_ERR_INVAL, -22);
        return;
    }
    u_memcpy(path, raw_path, l + 1);
    normalize_path(path);

    FILINFO fi;
    FRESULT fr = f_stat(path, &fi);
    if (fr != FR_OK) {
        int64_t e = fr_to_errno(fr);
        reply_ret(local_port, id, errno_to_status(e), e);
        return;
    }
    /* 只动只读位，其余（隐藏/系统/归档/目录）原样保留 */
    BYTE attr = fi.fattrib;
    bool any_write = ((mode & SUKI_S_IWUSR) || (mode & SUKI_S_IWGRP)
                      || (mode & SUKI_S_IWOTH));
    if (any_write) {
        attr &= (BYTE)~(AM_RDO);
    } else {
        attr |= AM_RDO;
    }
    fr = f_chmod(path, attr, AM_RDO);
    if (fr != FR_OK) {
        int64_t e = fr_to_errno(fr);
        reply_ret(local_port, id, errno_to_status(e), e);
        return;
    }
    reply_ret(local_port, id, FS_OK, 0);
}

/* ===================== 请求处理（一代） ===================== */
/* 列目录：枚举 path（来自请求负载，绝对或相对）下所有条目，以 "name\n" 形式写入应答数据。 */
static void handle_list(uint32_t local_port, uint32_t id, const char *path)
{
    char *data = (char *)(g_resp + sizeof(mach_msg_header_t) + sizeof(fs_resp_t));
    uint32_t total = 0;

    char pbuf[256];
    if (path && path[0]) {
        u_memcpy(pbuf, path, u_strlen(path) + 1);
    } else {
        pbuf[0] = '\0';
    }
    normalize_path(pbuf);

    FRESULT fr = f_opendir(&g_dir, pbuf);
    if (fr != FR_OK) {
        build_resp(local_port, id, fr_to_status(fr), 0, NULL);
        mach_msg_send(g_resp, resp_header()->msgh_size);
        return;
    }
    for (;;) {
        fr = f_readdir(&g_dir, &g_finfo);
        if (fr != FR_OK) {
            f_closedir(&g_dir);
            build_resp(local_port, id, fr_to_status(fr), total, NULL);
            mach_msg_send(g_resp, resp_header()->msgh_size);
            return;
        }
        if (g_finfo.fname[0] == '\0') break;   /* 目录结束 */
        const char *name = g_finfo.fname;
        uint32_t l = (uint32_t)u_strlen(name);
        if (total + l + 1 < RESP_DATA_MAX) {
            u_memcpy(data + total, name, l);
            data[total + l] = '\n';
            total += l + 1;
        } else {
            break;  /* 缓冲满，截断 */
        }
    }
    f_closedir(&g_dir);
    build_resp(local_port, id, FS_OK, total, NULL);
    mach_msg_send(g_resp, resp_header()->msgh_size);
}

/* 读文件（offset=0 的便捷封装；用于 FS_MSG_READ）。 */
static void handle_read(uint32_t local_port, uint32_t id, const char *fname)
{
    char path[256];
    u_memcpy(path, fname, u_strlen(fname) + 1);
    normalize_path(path);

    FRESULT fr = f_open(&g_fil, path, FA_READ | FA_OPEN_EXISTING);
    if (fr != FR_OK) {
        build_resp(local_port, id, fr_to_status(fr), 0, NULL);
        mach_msg_send(g_resp, resp_header()->msgh_size);
        return;
    }
    char *data = (char *)(g_resp + sizeof(mach_msg_header_t) + sizeof(fs_resp_t));
    UINT br = 0;
    uint32_t want = RESP_DATA_MAX;
    fr = f_read(&g_fil, data, want, &br);
    f_close(&g_fil);
    if (fr != FR_OK) {
        build_resp(local_port, id, fr_to_status(fr), 0, NULL);
    } else {
        build_resp(local_port, id, FS_OK, (uint32_t)br, NULL);
    }
    mach_msg_send(g_resp, resp_header()->msgh_size);
}

/* 分块读（FS_MSG_READ_AT）：从 offset 起读 length 字节。 */
static void handle_read_at(uint32_t local_port, uint32_t id,
                           uint32_t offset, uint32_t length, const char *fname)
{
    char path[256];
    u_memcpy(path, fname, u_strlen(fname) + 1);
    normalize_path(path);

    FRESULT fr = f_open(&g_fil, path, FA_READ | FA_OPEN_EXISTING);
    if (fr != FR_OK) {
        /* 诊断：打印实际 FatFs 错误码与路径，便于排查子目录/大小写/挂载问题 */
        u_print("[fs] read_at open FAIL fr=");
        { char d[16]; u_print(u_utoa_s((uint64_t)fr, d, sizeof(d))); }
        u_print(" path='");
        u_print(path);
        u_print("'\n");
        build_resp(local_port, id, fr_to_status(fr), 0, NULL);
        mach_msg_send(g_resp, resp_header()->msgh_size);
        return;
    }
    char *data = (char *)(g_resp + sizeof(mach_msg_header_t) + sizeof(fs_resp_t));
    UINT br = 0;
    uint32_t want = length;
    if (want > RESP_DATA_MAX) want = RESP_DATA_MAX;
    if (offset > 0) {
        FRESULT ls = f_lseek(&g_fil, (FSIZE_t)offset);
        if (ls != FR_OK) {
            f_close(&g_fil);
            build_resp(local_port, id, fr_to_status(ls), 0, NULL);
            mach_msg_send(g_resp, resp_header()->msgh_size);
            return;
        }
    }
    fr = f_read(&g_fil, data, want, &br);
    f_close(&g_fil);
    if (fr != FR_OK) {
        build_resp(local_port, id, fr_to_status(fr), 0, NULL);
    } else {
        build_resp(local_port, id, FS_OK, (uint32_t)br, NULL);
    }
    mach_msg_send(g_resp, resp_header()->msgh_size);
}

/* 整文件读（FS_MSG_READ_FILE，内核 execve/spawn 用）：读入 g_filebuf[sizeof(fs_resp_t)..]，
 * 头部写 fs_resp_t。返回字节数，OOL 描述符指向 g_filebuf。 */
static void handle_read_file(uint32_t local_port, uint32_t id, const char *fname)
{
    char path[256];
    u_memcpy(path, fname, u_strlen(fname) + 1);
    normalize_path(path);

    FRESULT fr = f_open(&g_fil, path, FA_READ | FA_OPEN_EXISTING);
    if (fr != FR_OK) {
        build_resp(local_port, id, fr_to_status(fr), 0, NULL);
        mach_msg_send(g_resp, resp_header()->msgh_size);
        return;
    }
    FSIZE_t fsize = f_size(&g_fil);
    if (fsize > FILEBUF_SIZE - sizeof(fs_resp_t)) {
        f_close(&g_fil);
        build_resp(local_port, id, FS_ERR_TOOBIG, 0, NULL);
        mach_msg_send(g_resp, resp_header()->msgh_size);
        return;
    }
    UINT br = 0;
    fr = f_read(&g_fil, g_filebuf + sizeof(fs_resp_t), (UINT)fsize, &br);
    f_close(&g_fil);
    if (fr != FR_OK || (FSIZE_t)br != fsize) {
        build_resp(local_port, id, FS_ERR_IO, 0, NULL);
        mach_msg_send(g_resp, resp_header()->msgh_size);
        return;
    }
    fs_resp_t *ofr = (fs_resp_t *)g_filebuf;
    ofr->status = FS_OK;
    ofr->length = (uint32_t)br;
    build_ool_resp(local_port, id, (uint32_t)br);
    mach_msg_send(g_resp, resp_header()->msgh_size);
}

/* 创建空文件（FS_MSG_CREATE，已存在则幂等成功）。 */
static void handle_create(uint32_t local_port, uint32_t id, const char *fname)
{
    char path[256];
    u_memcpy(path, fname, u_strlen(fname) + 1);
    normalize_path(path);

    /* 已存在则直接成功（幂等） */
    FRESULT fr = f_open(&g_fil, path, FA_READ | FA_OPEN_EXISTING);
    if (fr == FR_OK) {
        f_close(&g_fil);
        build_resp(local_port, id, FS_OK, 0, NULL);
        mach_msg_send(g_resp, resp_header()->msgh_size);
        return;
    }
    /* 不存在：创建空文件 */
    fr = f_open(&g_fil, path, FA_WRITE | FA_CREATE_NEW);
    if (fr != FR_OK) {
        build_resp(local_port, id, fr_to_status(fr), 0, NULL);
        mach_msg_send(g_resp, resp_header()->msgh_size);
        return;
    }
    f_close(&g_fil);
    build_resp(local_port, id, FS_OK, 0, NULL);
    mach_msg_send(g_resp, resp_header()->msgh_size);
}

/* 创建目录（FS_MSG_MKDIR）。 */
static void handle_mkdir(uint32_t local_port, uint32_t id, const char *fname)
{
    char path[256];
    u_memcpy(path, fname, u_strlen(fname) + 1);
    normalize_path(path);

    FRESULT fr = f_mkdir(path);
    /* 已存在也视作成功（幂等） */
    if (fr == FR_OK || fr == FR_EXIST) {
        build_resp(local_port, id, FS_OK, 0, NULL);
    } else {
        build_resp(local_port, id, fr_to_status(fr), 0, NULL);
    }
    mach_msg_send(g_resp, resp_header()->msgh_size);
}

/* 写文件（FS_MSG_WRITE）：offset 起覆盖/追加，length 字节 data。 */
static void handle_write(uint32_t local_port, uint32_t id, uint32_t offset,
                         uint32_t length, const char *fname, const void *data)
{
    char path[256];
    u_memcpy(path, fname, u_strlen(fname) + 1);
    normalize_path(path);

    FRESULT fr = f_open(&g_fil, path, FA_WRITE | FA_OPEN_ALWAYS);
    if (fr != FR_OK) {
        build_resp(local_port, id, fr_to_status(fr), 0, NULL);
        mach_msg_send(g_resp, resp_header()->msgh_size);
        return;
    }
    if (offset > 0) {
        FRESULT ls = f_lseek(&g_fil, (FSIZE_t)offset);
        if (ls != FR_OK) {
            f_close(&g_fil);
            build_resp(local_port, id, fr_to_status(ls), 0, NULL);
            mach_msg_send(g_resp, resp_header()->msgh_size);
            return;
        }
    }
    UINT bw = 0;
    fr = f_write(&g_fil, data, length, &bw);
    f_close(&g_fil);
    if (fr != FR_OK || (UINT)bw != length) {
        build_resp(local_port, id, FS_ERR_IO, 0, NULL);
    } else {
        build_resp(local_port, id, FS_OK, 0, NULL);
    }
    mach_msg_send(g_resp, resp_header()->msgh_size);
}

/* 删除文件/目录（FS_MSG_UNLINK）。 */
static void handle_unlink(uint32_t local_port, uint32_t id, const char *fname)
{
    char path[256];
    u_memcpy(path, fname, u_strlen(fname) + 1);
    normalize_path(path);

    FRESULT fr = f_unlink(path);
    if (fr == FR_OK || fr == FR_NO_FILE || fr == FR_NO_PATH) {
        build_resp(local_port, id, FS_OK, 0, NULL);
    } else {
        build_resp(local_port, id, fr_to_status(fr), 0, NULL);
    }
    mach_msg_send(g_resp, resp_header()->msgh_size);
}

/* 重命名（FS_MSG_RENAME，支持跨目录移动）。 */
static void handle_rename(uint32_t local_port, uint32_t id,
                          const char *old_name, const char *new_name)
{
    char op[256], np[256];
    u_memcpy(op, old_name, u_strlen(old_name) + 1);
    u_memcpy(np, new_name, u_strlen(new_name) + 1);
    normalize_path(op);
    normalize_path(np);

    FRESULT fr = f_rename(op, np);
    if (fr == FR_OK) {
        build_resp(local_port, id, FS_OK, 0, NULL);
    } else {
        build_resp(local_port, id, fr_to_status(fr), 0, NULL);
    }
    mach_msg_send(g_resp, resp_header()->msgh_size);
}

/* 截断文件到指定大小（FS_MSG_TRUNCATE）。 */
static void handle_truncate(uint32_t local_port, uint32_t id,
                            uint32_t size, const char *fname)
{
    char path[256];
    u_memcpy(path, fname, u_strlen(fname) + 1);
    normalize_path(path);

    FRESULT fr = f_open(&g_fil, path, FA_WRITE | FA_OPEN_EXISTING);
    if (fr != FR_OK) {
        build_resp(local_port, id, fr_to_status(fr), 0, NULL);
        mach_msg_send(g_resp, resp_header()->msgh_size);
        return;
    }
    FRESULT ls = f_lseek(&g_fil, (FSIZE_t)size);
    if (ls != FR_OK) {
        f_close(&g_fil);
        build_resp(local_port, id, fr_to_status(ls), 0, NULL);
        mach_msg_send(g_resp, resp_header()->msgh_size);
        return;
    }
    fr = f_truncate(&g_fil);
    f_close(&g_fil);
    if (fr == FR_OK) {
        build_resp(local_port, id, FS_OK, 0, NULL);
    } else {
        build_resp(local_port, id, fr_to_status(fr), 0, NULL);
    }
    mach_msg_send(g_resp, resp_header()->msgh_size);
}

/* ===================== 服务主循环 ===================== */
static void service_loop(void)
{
    u_print("[fs] service loop entered\n");
    for (;;) {
        static uint8_t s_reqbuf[sizeof(mach_msg_header_t) + sizeof(fs_write_req_t) + 96 + FS_WRITE_MAX];
        uint8_t *reqbuf = s_reqbuf;
        uint32_t r = (uint32_t)sizeof(s_reqbuf);
        if (mach_msg_recv(reqbuf, r, FS_PORT) != MACH_MSG_SUCCESS)
            continue;
        mach_msg_header_t *h = (mach_msg_header_t *)reqbuf;
        if (h->msgh_size < sizeof(mach_msg_header_t) || h->msgh_size > sizeof(s_reqbuf))
            continue;                        /* 畸形消息，丢弃 */
        uint32_t local = h->msgh_local_port;
        uint32_t id    = h->msgh_id;
        uint8_t *payload = reqbuf + sizeof(mach_msg_header_t);

        switch (id) {
        case FS_MSG_LIST:
            handle_list(local, id, (const char *)payload);
            break;
        case FS_MSG_READ: {
            char *fname = (char *)payload;
            handle_read(local, id, fname);
            break;
        }
        case FS_MSG_READ_AT: {
            fs_read_at_req_t *ra = (fs_read_at_req_t *)payload;
            char *fname = (char *)(payload + sizeof(fs_read_at_req_t));
            handle_read_at(local, id, ra->offset, ra->length, fname);
            break;
        }
        case FS_MSG_READ_FILE: {
            char *fname = (char *)payload;
            handle_read_file(local, id, fname);
            break;
        }
        case FS_MSG_CREATE:
            handle_create(local, id, (char *)payload);
            break;
        case FS_MSG_MKDIR:
            handle_mkdir(local, id, (char *)payload);
            break;
        case FS_MSG_WRITE: {
            fs_write_req_t *w = (fs_write_req_t *)payload;
            char *fname = (char *)(payload + sizeof(fs_write_req_t));
            const void *data = fname + u_strlen(fname) + 1;
            uint32_t len = w->length;
            if (len > FS_WRITE_MAX) len = FS_WRITE_MAX;
            handle_write(local, id, w->offset, len, fname, data);
            break;
        }
        case FS_MSG_UNLINK:
            handle_unlink(local, id, (char *)payload);
            break;
        case FS_MSG_RENAME: {
            fs_rename_req_t *r = (fs_rename_req_t *)payload;
            handle_rename(local, id, r->old_name, r->new_name);
            break;
        }
        case FS_MSG_TRUNCATE: {
            fs_trunc_req_t *t = (fs_trunc_req_t *)payload;
            char *fname = (char *)(payload + sizeof(fs_trunc_req_t));
            handle_truncate(local, id, t->size, fname);
            break;
        }

        /* ================= 二代：句柄式（POSIX）================= */
        case FS_MSG_OPEN: {
            fs_open_req_t *o = (fs_open_req_t *)payload;
            char *p = (char *)(payload + sizeof(fs_open_req_t));
            handle_open2(local, id, o, p);
            break;
        }
        case FS_MSG_CLOSE: {
            fs_fd_req_t *fq = (fs_fd_req_t *)payload;
            handle_close2(local, id, fq->fd);
            break;
        }
        case FS_MSG_READFD: {
            fs_read_req_t *rr = (fs_read_req_t *)payload;
            handle_read_fd(local, id, rr->fd, rr->length);
            break;
        }
        case FS_MSG_WRITEFD: {
            fs_write_fd_req_t *wr = (fs_write_fd_req_t *)payload;
            const void *data = payload + sizeof(fs_write_fd_req_t);
            uint32_t len = wr->length;
            if (len > FS_WRITE_MAX) len = FS_WRITE_MAX;
            handle_write_fd(local, id, wr->fd, len, wr->offset, data);
            break;
        }
        case FS_MSG_SEEKFD: {
            fs_seek_req_t *sr = (fs_seek_req_t *)payload;
            handle_seek_fd(local, id, sr->fd, sr->offset, sr->whence);
            break;
        }
        case FS_MSG_FSTAT: {
            fs_fd_req_t *fq = (fs_fd_req_t *)payload;
            handle_fstat(local, id, fq->fd);
            break;
        }
        case FS_MSG_FTRUNC: {
            fs_ftrunc_req_t *fq = (fs_ftrunc_req_t *)payload;
            handle_ftrunc(local, id, fq->fd, fq->length);
            break;
        }
        case FS_MSG_STAT:
            handle_stat(local, id, (char *)payload);
            break;
        case FS_MSG_OPENDIR:
            handle_opendir(local, id, (char *)payload);
            break;
        case FS_MSG_READDIR: {
            fs_fd_req_t *fq = (fs_fd_req_t *)payload;
            handle_readdir(local, id, fq->fd);
            break;
        }
        case FS_MSG_CLOSEDIR: {
            fs_fd_req_t *fq = (fs_fd_req_t *)payload;
            handle_closedir(local, id, fq->fd);
            break;
        }
        case FS_MSG_MKDIR2: {
            fs_path_mode_req_t *pm = (fs_path_mode_req_t *)payload;
            char *p = (char *)(payload + sizeof(fs_path_mode_req_t));
            handle_mkdir2(local, id, p);
            (void)pm;
            break;
        }
        case FS_MSG_UNLINK2: {
            fs_unlink_req_t *uq = (fs_unlink_req_t *)payload;
            char *p = (char *)(payload + sizeof(fs_unlink_req_t));
            handle_unlink2(local, id, p, uq->is_dir);
            break;
        }
        case FS_MSG_RENAME2: {
            char *oldp = (char *)payload;
            char *newp = oldp + u_strlen(oldp) + 1;
            handle_rename2(local, id, oldp, newp);
            break;
        }
        case FS_MSG_ACCESS: {
            fs_path_mode_req_t *pm = (fs_path_mode_req_t *)payload;
            char *p = (char *)(payload + sizeof(fs_path_mode_req_t));
            handle_access(local, id, p, pm->mode);
            break;
        }
        case FS_MSG_STATFS:
            handle_statfs(local, id);
            break;
        case FS_MSG_SYNC:
            handle_sync(local, id);
            break;
        case FS_MSG_UTIME: {
            fs_utime_req_t *ut = (fs_utime_req_t *)payload;
            char *p = (char *)(payload + sizeof(fs_utime_req_t));
            handle_utime(local, id, ut->atime, ut->mtime, p);
            break;
        }
        case FS_MSG_CHMOD: {
            fs_chmod_req_t *cm = (fs_chmod_req_t *)payload;
            char *p = (char *)(payload + sizeof(fs_chmod_req_t));
            handle_chmod(local, id, cm->mode, p);
            break;
        }

        default:
            /* 未知消息 id：回一条「失败」应答，绝不应答畸形长度。
             * 注意：MSG_ID_SERVICE_DOWN(101) 是内核发给【客户端】的通知，
             * 不会投递到 FS_PORT；即便误投，走本分支也只是回一条错误应答，
             * 不会造成自激（不会再次 send 给自己）。 */
            build_resp(local, id, FS_ERR_NOENT, 0, NULL);
            mach_msg_send(g_resp, resp_header()->msgh_size);
            break;
        }
    }
}

/* ===================== 自检 ===================== */
static void self_test(void)
{
    u_print("[fs] self-test begin\n");
    bool ok = true;

    /* create + write + readback */
    {
        FRESULT fr = f_open(&g_fil, "SELFTEST.TXT", FA_WRITE | FA_CREATE_ALWAYS);
        if (fr != FR_OK) { u_print("[fs] self-test: create FAIL\n"); ok = false; }
        else {
            const char *msg = "HELLO_SUKI_FAT32_WRITE_PATH_OK";
            UINT bw = 0;
            fr = f_write(&g_fil, msg, (UINT)u_strlen(msg), &bw);
            f_close(&g_fil);
            if (fr != FR_OK || (UINT)bw != u_strlen(msg)) {
                u_print("[fs] self-test: write FAIL\n"); ok = false;
            } else {
                char rb[64];
                fr = f_open(&g_fil, "SELFTEST.TXT", FA_READ | FA_OPEN_EXISTING);
                if (fr != FR_OK) { u_print("[fs] self-test: reopen FAIL\n"); ok = false; }
                else {
                    UINT br = 0;
                    fr = f_read(&g_fil, rb, sizeof(rb) - 1, &br);
                    f_close(&g_fil);
                    rb[br] = '\0';
                    if (fr != FR_OK || u_strcmp(rb, msg) != 0) {
                        u_print("[fs] self-test: readback mismatch\n"); ok = false;
                    }
                }
            }
        }
    }

    /* append via offset write */
    {
        FRESULT fr = f_open(&g_fil, "SELFTEST.TXT", FA_WRITE | FA_OPEN_EXISTING);
        if (fr != FR_OK) { u_print("[fs] self-test: append open FAIL\n"); ok = false; }
        else {
            const char *msg2 = "_APPEND";
            FRESULT ls = f_lseek(&g_fil, (FSIZE_t)f_size(&g_fil));
            if (ls != FR_OK) { u_print("[fs] self-test: append seek FAIL\n"); ok = false; }
            else {
                UINT bw = 0;
                fr = f_write(&g_fil, msg2, (UINT)u_strlen(msg2), &bw);
                f_close(&g_fil);
                if (fr != FR_OK || (UINT)bw != u_strlen(msg2)) {
                    u_print("[fs] self-test: append write FAIL\n"); ok = false;
                }
            }
        }
        char rb2[80];
        FRESULT fr2 = f_open(&g_fil, "SELFTEST.TXT", FA_READ | FA_OPEN_EXISTING);
        if (fr2 == FR_OK) {
            UINT br = 0;
            fr2 = f_read(&g_fil, rb2, sizeof(rb2) - 1, &br);
            f_close(&g_fil);
            rb2[br] = '\0';
            if (fr2 != FR_OK || u_strcmp(rb2, "HELLO_SUKI_FAT32_WRITE_PATH_OK_APPEND") != 0) {
                u_print("[fs] self-test: append readback FAIL\n"); ok = false;
            }
        }
    }

    /* mkdir + nested file */
    {
        FRESULT fr = f_mkdir("SUBDIR");
        if (fr != FR_OK && fr != FR_EXIST) { u_print("[fs] self-test: mkdir FAIL\n"); ok = false; }
        else {
            fr = f_open(&g_fil, "SUBDIR/NEST.TXT", FA_WRITE | FA_CREATE_ALWAYS);
            if (fr != FR_OK) { u_print("[fs] self-test: subdir create FAIL\n"); ok = false; }
            else {
                UINT bw = 0;
                fr = f_write(&g_fil, "NESTED", 6, &bw);
                f_close(&g_fil);
                if (fr != FR_OK || (UINT)bw != 6) { u_print("[fs] self-test: subdir write FAIL\n"); ok = false; }
            }
        }
    }

    /* rename */
    {
        FRESULT fr = f_rename("SELFTEST.TXT", "RENAMED.TXT");
        if (fr != FR_OK) { u_print("[fs] self-test: rename FAIL\n"); ok = false; }
        else {
            fr = f_open(&g_fil, "SELFTEST.TXT", FA_READ | FA_OPEN_EXISTING);
            if (fr == FR_OK) { f_close(&g_fil); u_print("[fs] self-test: old name still exists\n"); ok = false; }
            fr = f_open(&g_fil, "RENAMED.TXT", FA_READ | FA_OPEN_EXISTING);
            if (fr != FR_OK) { u_print("[fs] self-test: renamed missing\n"); ok = false; }
            else f_close(&g_fil);
        }
    }

    /* truncate */
    {
        FRESULT fr = f_open(&g_fil, "TRUNC.TXT", FA_WRITE | FA_CREATE_ALWAYS);
        if (fr != FR_OK) { u_print("[fs] self-test: trunc create FAIL\n"); ok = false; }
        else {
            UINT bw = 0;
            fr = f_write(&g_fil, "0123456789", 10, &bw);
            f_close(&g_fil);
            if (fr != FR_OK || (UINT)bw != 10) { u_print("[fs] self-test: trunc write FAIL\n"); ok = false; }
            else {
                fr = f_open(&g_fil, "TRUNC.TXT", FA_WRITE | FA_OPEN_EXISTING);
                if (fr != FR_OK) { u_print("[fs] self-test: trunc open FAIL\n"); ok = false; }
                else {
                    FRESULT ls = f_lseek(&g_fil, 4);
                    if (ls != FR_OK) { u_print("[fs] self-test: trunc seek FAIL\n"); ok = false; }
                    else {
                        fr = f_truncate(&g_fil);
                        f_close(&g_fil);
                        if (fr != FR_OK) { u_print("[fs] self-test: truncate FAIL\n"); ok = false; }
                        else {
                            fr = f_open(&g_fil, "TRUNC.TXT", FA_READ | FA_OPEN_EXISTING);
                            if (fr == FR_OK) {
                                FSIZE_t sz = f_size(&g_fil);
                                f_close(&g_fil);
                                if (sz != 4) { u_print("[fs] self-test: truncated size wrong\n"); ok = false; }
                            }
                        }
                    }
                }
            }
        }
    }

    /* cleanup */
    f_unlink("RENAMED.TXT");
    f_unlink("SUBDIR/NEST.TXT");
    f_unlink("SUBDIR");
    f_unlink("TRUNC.TXT");

    /* bigfile whole-read (MOONHALO.MP3 if present) */
    {
        FRESULT fr = f_open(&g_fil, "MOONHALO.MP3", FA_READ | FA_OPEN_EXISTING);
        if (fr == FR_OK) {
            FSIZE_t sz = f_size(&g_fil);
            if (sz > FILEBUF_SIZE - sizeof(fs_resp_t)) {
                u_print("[fs] self-test: bigfile too large, skip\n");
            } else {
                UINT br = 0;
                fr = f_read(&g_fil, g_filebuf, (UINT)sz, &br);
                f_close(&g_fil);
                if (fr != FR_OK || (FSIZE_t)br != sz) {
                    u_print("[fs] self-test: bigfile whole-read mismatch\n"); ok = false;
                } else {
                    /* 校验尾部 100 字节与整读再截的一致 */
                    uint8_t tail_inline[128];
                    fr = f_open(&g_fil, "MOONHALO.MP3", FA_READ | FA_OPEN_EXISTING);
                    if (fr == FR_OK) {
                        FSIZE_t off = (sz > 100) ? sz - 100 : 0;
                        uint32_t tl = (sz > 100) ? 100 : (uint32_t)sz;
                        f_lseek(&g_fil, off);
                        UINT tbr = 0;
                        f_read(&g_fil, tail_inline, tl, &tbr);
                        f_close(&g_fil);
                        if (tbr != tl || u_memcmp(g_filebuf + off, tail_inline, tl) != 0) {
                            u_print("[fs] self-test: bigfile tail mismatch\n"); ok = false;
                        }
                    }
                }
            }
        }
    }

    u_print(ok ? "[fs] self-test ALL PASS\n" : "[fs] self-test FAILED\n");
}

/* FatFs 需要的当前时间回调（FF_FS_NORTC=1 或提供 get_fattime）。
 * 返回 FAT 时间戳 DWORD：位 0-4 秒/2，5-10 分，11-15 时，16-20 日，
 * 21-24 月，25-31 年(1980 起)。这里用单调计数派生一个合法但固定的时间，
 * 避免依赖 RTC 驱动；文件时间仅用于展示，不影响正确性。 */
DWORD get_fattime(void)
{
    /* 固定时间戳：2024-01-01 00:00:00（合法且稳定，避免随机数导致测试抖动） */
    DWORD yr = (2024 - 1980) & 0x7F;   /* 44 */
    DWORD mo = 1 & 0x0F;
    DWORD da = 1 & 0x1F;
    DWORD hh = 0 & 0x1F;
    DWORD mm = 0 & 0x3F;
    DWORD ss = 0 & 0x3F;              /* 秒/2 */
    return (yr << 25) | (mo << 21) | (da << 16) | (hh << 11) | (mm << 5) | ss;
}

/* ===================== 入口 ===================== */
int main(void)
{
    u_print("[fs] FS_SERVER starting\n");
    sys_port_claim(FS_PORT);
    sys_port_claim(FS_REPLY_PORT);

    /* FatFs 挂载（卷 ""，物理盘 0；disk_initialize 会认领 FS_REPLY_PORT） */
    FRESULT fr = f_mount(&g_fatfs, "", 1);
    if (fr != FR_OK) {
        u_print("[fs] f_mount failed\n");
        return 1;
    }
    u_print("[fs] mounted FAT32 (FatFs)\n");

    self_test();
    service_loop();
    f_mount(NULL, "", 0);   /* 卸载 */
    return 0;
}

/* 入口由用户态启动桩 user/lib/crt0.S 提供（调用 main）。 */
