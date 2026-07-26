/*
 * user/fs_server.c
 * -----------------------------------------------------------------------------
 * FS_SERVER：Ring3 FAT32 文件系统服务（只读）。
 *
 * 红线（手册 3 章）：FAT32 解析完全在用户态。磁盘扇区通过 mach_msg
 * 发往内核 DISK_PORT 获取（应答收在 FS_REPLY_PORT）。
 *
 * 启动：挂载卷（读 BPB）-> 自检列根目录 -> 进入服务循环：
 *   FS_MSG_LIST -> 返回根目录文本列表
 *   FS_MSG_READ -> 按 8.3 名查找并返回文件内容（<= FS_DATA_MAX）
 */
#include "lib/suki.h"
#include <ipc/disk_proto.h>
#include <ipc/fs_proto.h>

#define SECTOR 512

/* ---- 与内核 disk-srv 的通信 ---- */
static uint8_t g_diskbuf[sizeof(mach_msg_header_t) + sizeof(disk_read_resp_t)
                         + DISK_MAX_SECTORS * SECTOR];

static bool disk_read(uint32_t lba, uint32_t count, void *out)
{
    struct {
        mach_msg_header_t h;
        disk_read_req_t   r;
    } req;
    req.h.msgh_bits = 0;
    req.h.msgh_size = sizeof(req);
    req.h.msgh_remote_port = DISK_PORT;
    req.h.msgh_local_port = FS_REPLY_PORT;   /* 应答端口 */
    req.h.msgh_id = DISK_MSG_READ;
    req.h.msgh_reserved = 0;
    req.r.lba = lba;
    req.r.count = count;
    req.r.pad = 0;

    if (mach_msg_send(&req, sizeof(req)) != MACH_MSG_SUCCESS) {
        return false;
    }
    if (mach_msg_recv(g_diskbuf, sizeof(g_diskbuf), FS_REPLY_PORT)
            != MACH_MSG_SUCCESS) {
        return false;
    }
    disk_read_resp_t *rr =
        (disk_read_resp_t *)(g_diskbuf + sizeof(mach_msg_header_t));
    if (rr->status != 0) {
        return false;
    }
    u_memcpy(out, g_diskbuf + sizeof(mach_msg_header_t) + sizeof(*rr),
             count * SECTOR);
    return true;
}

/* ---- FAT32 卷参数 ---- */
static uint32_t g_sec_per_clus, g_fat_begin, g_data_begin, g_root_clus;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool fat32_mount(void)
{
    uint8_t bpb[SECTOR];
    if (!disk_read(0, 1, bpb)) {
        return false;
    }
    uint16_t byts_per_sec = rd16(bpb + 11);
    g_sec_per_clus        = bpb[13];
    uint16_t rsvd         = rd16(bpb + 14);
    uint8_t  nfats        = bpb[16];
    uint32_t fatsz32      = rd32(bpb + 36);
    g_root_clus           = rd32(bpb + 44);

    if (byts_per_sec != SECTOR || g_sec_per_clus == 0 || fatsz32 == 0) {
        return false;
    }
    g_fat_begin  = rsvd;
    g_data_begin = rsvd + nfats * fatsz32;

    char n[24];
    u_print("[fs] FAT32 mounted: spc=");
    u_print(u_utoa(g_sec_per_clus, n));
    u_print(" fat@");
    u_print(u_utoa(g_fat_begin, n));
    u_print(" data@");
    u_print(u_utoa(g_data_begin, n));
    u_print(" root_clus=");
    u_print(u_utoa(g_root_clus, n));
    u_print("\n");
    return true;
}

static uint32_t clus_to_lba(uint32_t clus)
{
    return g_data_begin + (clus - 2) * g_sec_per_clus;
}

static uint32_t fat_next(uint32_t clus)
{
    uint8_t sec[SECTOR];
    uint32_t lba = g_fat_begin + (clus * 4) / SECTOR;
    if (!disk_read(lba, 1, sec)) {
        return 0x0FFFFFFF;
    }
    return rd32(sec + (clus * 4) % SECTOR) & 0x0FFFFFFF;
}

/* 目录项 -> "NAME.EXT"（返回长度） */
static int fmt_83(const uint8_t *e, char *out)
{
    int n = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) {
        out[n++] = (char)e[i];
    }
    if (e[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) {
            out[n++] = (char)e[i];
        }
    }
    out[n] = '\0';
    return n;
}

/* 遍历根目录：cb 返回 true 则停止。cb(条目, name, 私有指针) */
typedef bool (*dir_cb)(const uint8_t *entry, const char *name, void *priv);

static void walk_root(dir_cb cb, void *priv)
{
    uint8_t sec[SECTOR];
    uint32_t clus = g_root_clus;
    while (clus >= 2 && clus < 0x0FFFFFF8) {
        for (uint32_t s = 0; s < g_sec_per_clus; s++) {
            if (!disk_read(clus_to_lba(clus) + s, 1, sec)) {
                return;
            }
            for (int off = 0; off < SECTOR; off += 32) {
                const uint8_t *e = sec + off;
                if (e[0] == 0x00) {
                    return;                    /* 目录结束 */
                }
                if (e[0] == 0xE5 || (e[11] & 0x0F) == 0x0F || (e[11] & 0x08)) {
                    continue;                  /* 删除项/LFN/卷标 */
                }
                char name[16];
                fmt_83(e, name);
                if (cb(e, name, priv)) {
                    return;
                }
            }
        }
        clus = fat_next(clus);
    }
}

/* ---- LIST ---- */
struct list_ctx { char *buf; uint32_t len; };

static bool list_cb(const uint8_t *e, const char *name, void *priv)
{
    struct list_ctx *c = (struct list_ctx *)priv;
    uint32_t size = rd32(e + 28);
    bool is_dir = (e[11] & 0x10) != 0;

    char line[64];
    int n = 0;
    const char *p = name;
    while (*p) {
        line[n++] = *p++;
    }
    while (n < 14) {
        line[n++] = ' ';
    }
    if (is_dir) {
        const char *d = "<DIR>";
        while (*d) {
            line[n++] = *d++;
        }
    } else {
        char num[24];
        u_utoa(size, num);
        const char *q = num;
        while (*q) {
            line[n++] = *q++;
        }
        const char *u = " bytes";
        while (*u) {
            line[n++] = *u++;
        }
    }
    line[n++] = '\n';

    if (c->len + (uint32_t)n >= FS_DATA_MAX) {
        return true;
    }
    u_memcpy(c->buf + c->len, line, (size_t)n);
    c->len += (uint32_t)n;
    return false;
}

/* ---- READ ---- */
struct find_ctx { const char *want; uint32_t clus, size; bool found; };

static bool find_cb(const uint8_t *e, const char *name, void *priv)
{
    struct find_ctx *c = (struct find_ctx *)priv;
    if (u_strcmp(name, c->want) == 0 && !(e[11] & 0x10)) {
        c->clus = ((uint32_t)rd16(e + 20) << 16) | rd16(e + 26);
        c->size = rd32(e + 28);
        c->found = true;
        return true;
    }
    return false;
}

static uint32_t read_file(struct find_ctx *f, char *out, uint32_t cap)
{
    uint8_t sec[SECTOR];
    uint32_t done = 0;
    uint32_t clus = f->clus;
    uint32_t remain = f->size < cap ? f->size : cap;
    while (remain > 0 && clus >= 2 && clus < 0x0FFFFFF8) {
        for (uint32_t s = 0; s < g_sec_per_clus && remain > 0; s++) {
            if (!disk_read(clus_to_lba(clus) + s, 1, sec)) {
                return done;
            }
            uint32_t n = remain < SECTOR ? remain : SECTOR;
            u_memcpy(out + done, sec, n);
            done += n;
            remain -= n;
        }
        clus = fat_next(clus);
    }
    return done;
}

/* ---- 服务循环 ---- */
static uint8_t g_req[512];
static uint8_t g_resp[sizeof(mach_msg_header_t) + sizeof(fs_resp_t) + FS_DATA_MAX];

static void serve(void)
{
    for (;;) {
        if (mach_msg_recv(g_req, sizeof(g_req), FS_PORT) != MACH_MSG_SUCCESS) {
            continue;
        }
        mach_msg_header_t *rh = (mach_msg_header_t *)g_req;
        uint32_t reply = rh->msgh_local_port;
        if (reply == 0) {
            continue;
        }

        mach_msg_header_t *h = (mach_msg_header_t *)g_resp;
        fs_resp_t *fr = (fs_resp_t *)(g_resp + sizeof(*h));
        char *data = (char *)g_resp + sizeof(*h) + sizeof(*fr);
        fr->status = FS_OK;
        fr->length = 0;

        if (rh->msgh_id == FS_MSG_LIST) {
            struct list_ctx c = { data, 0 };
            walk_root(list_cb, &c);
            fr->length = c.len;
        } else if (rh->msgh_id == FS_MSG_READ) {
            char *name = (char *)g_req + sizeof(*rh);
            g_req[sizeof(g_req) - 1] = 0;      /* 保证 NUL 终止 */
            struct find_ctx f = { name, 0, 0, false };
            walk_root(find_cb, &f);
            if (!f.found) {
                fr->status = FS_ERR_NOENT;
            } else {
                fr->length = read_file(&f, data, FS_DATA_MAX);
            }
        } else {
            fr->status = FS_ERR_IO;
        }

        uint32_t total = sizeof(*h) + sizeof(*fr) + fr->length;
        h->msgh_bits = 0;
        h->msgh_size = total;
        h->msgh_remote_port = reply;
        h->msgh_local_port = FS_PORT;
        h->msgh_id = rh->msgh_id;
        h->msgh_reserved = 0;
        mach_msg_send(g_resp, total);
    }
}

/* 挂载自检：把根目录列表直接打印到内核日志 */
static bool selftest_cb(const uint8_t *e, const char *name, void *priv)
{
    (void)priv;
    uint32_t size = rd32(e + 28);
    char num[24];
    u_print("[fs]   ");
    u_print(name);
    if (e[11] & 0x10) {
        u_print("  <DIR>");
    } else {
        u_print("  ");
        u_print(u_utoa(size, num));
        u_print(" bytes");
    }
    u_print("\n");
    return false;
}

int main(void)
{
    u_print("[fs] FS_SERVER starting (Ring3 FAT32, read-only)\n");
    /* A2 项：认领本服务的接收端口（否则内核会因 owner 不匹配拒绝接收） */
    sys_port_claim(FS_PORT);
    sys_port_claim(FS_REPLY_PORT);
    if (!fat32_mount()) {
        u_print("[fs] mount FAILED\n");
        return 1;
    }
    u_print("[fs] root directory (self-test):\n");
    walk_root(selftest_cb, 0);
    u_print("[fs] entering service loop on FS_PORT\n");
    serve();
    return 0;
}
