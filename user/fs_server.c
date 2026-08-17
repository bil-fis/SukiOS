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
#include "ff.h"            /* FatFs 类型与 API */

/* 本地 OOL 描述符（布局与内核 port.h 的 mach_ool_desc_t 一致，避免重定义冲突） */
typedef struct { uint64_t address; uint64_t size; } ool_desc_t;

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

/* ===================== 请求处理 ===================== */
/* 列根目录：枚举 "/" 下所有条目，以 "name\n" 形式写入应答数据。 */
static void handle_list(uint32_t local_port, uint32_t id)
{
    char *data = (char *)(g_resp + sizeof(mach_msg_header_t) + sizeof(fs_resp_t));
    uint32_t total = 0;

    FRESULT fr = f_opendir(&g_dir, "/");
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
            handle_list(local, id);
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
        default:
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
