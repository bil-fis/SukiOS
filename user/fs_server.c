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

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096UL
#endif

/* OOL 描述符（与内核 mach_ool_desc_t 二进制布局一致：address, size） */
typedef struct { uint64_t address; uint64_t size; } ool_desc_t;

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

/* FAT 扇区缓存：一个 FAT 扇区含 128 条 4 字节簇项。连续簇遍历若每次都
 * 重读 FAT 扇区，3877 簇的 walk 要 3877 次磁盘 PIO（QEMU 下约 20ms/次，
 * 累计 >70s，表现为“播放卡死”）。缓存最近一次 FAT 扇区后，同扇区内的簇
 * 项直接命中，walk 的磁盘读次数降为 ~30 次。 */
static uint32_t g_fat_cache_lba = 0xFFFFFFFFu;
static uint8_t  g_fat_cache[SECTOR];
static bool     g_fat_cache_ok = false;

static uint32_t fat_next(uint32_t clus)
{
    uint32_t lba = g_fat_begin + (clus * 4) / SECTOR;
    if (!g_fat_cache_ok || lba != g_fat_cache_lba) {
        if (!disk_read(lba, 1, g_fat_cache)) {
            return 0x0FFFFFFF;
        }
        g_fat_cache_lba = lba;
        g_fat_cache_ok  = true;
    }
    return rd32(g_fat_cache + (clus * 4) % SECTOR) & 0x0FFFFFFF;
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

/* read_dir 定义在下方（带 LFN 支持）；walk_root 复用之遍历根目录。 */
static void read_dir(uint32_t start_clus, dir_cb cb, void *priv);

static void walk_root(dir_cb cb, void *priv)
{
    read_dir(g_root_clus, cb, priv);
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

/* ---- 路径解析（支持根下子目录，如 BIN/HELLO.ELF） ---- */
typedef struct { const char *want; uint32_t clus, size; bool found, is_dir; } path_cb_t;

static void upcase_str(char *s)
{
    for (; *s; s++) {
        if (*s >= 'a' && *s <= 'z') {
            *s = (char)(*s - 'a' + 'A');
        }
    }
}

/* ---- 长文件名(LFN)支持 ----
 * 大于 8.3 的文件（如独立程序 ::BIN/PLAYAUDIO，9 字符）在 FAT32 上由
 * mtools 自动创建 LFN 条目（UTF-16LE，每条 13 字符，按逆序物理排列，
 * 末条序列号带 0x40 标志）。本模块在遍历目录时累加 LFN 条目，遇到紧随
 * 其后的 8.3 条目时重建出长名并传给 cb（无 LFN 则回退 8.3 短名）。
 *
 * 重建策略：利用序列号 S（1-based），把第 S 条 LFN 的 13 字符写入缓冲的
 * (S-1)*13 偏移处。由于物理顺序为逆序，按 (seq-1)*13 直接定位写入即可，
 * 无需关心到达顺序；长名长度 = 已见最大序列号 * 13。 */
#define LFN_CAP 256
static char   g_lfn[LFN_CAP];
static int    g_lfn_seq;     /* 当前累积的最大序列号 */
static bool   g_lfn_valid;

static void lfn_reset(void)
{
    g_lfn[0] = '\0';
    g_lfn_valid = false;
    g_lfn_seq = 0;
}

/* 解码一条 LFN 条目，写入 (seq-1)*13 处（UTF-16LE 的 ASCII 部分） */
static void lfn_add(const uint8_t *e)
{
    int seq = e[0] & 0x1F;                  /* 低 5 位 = 序列号(1-based) */
    if (seq == 0 || seq * 13 >= LFN_CAP) {
        return;
    }
    const uint8_t *chunk[3] = { e + 1, e + 14, e + 28 };
    int nbytes[3] = { 10, 12, 4 };          /* = 5+6+2 个 UTF-16 字符 */
    int pos = (seq - 1) * 13;
    for (int k = 0; k < 3; k++) {
        const uint8_t *base = chunk[k];
        int pairs = nbytes[k] / 2;
        for (int p = 0; p < pairs; p++) {
            uint16_t w = (uint16_t)base[p * 2] | ((uint16_t)base[p * 2 + 1] << 8);
            char ch = (w < 0x80) ? (char)w : '?';
            if (pos < LFN_CAP - 1) {
                g_lfn[pos++] = ch;
            }
        }
    }
    g_lfn_valid = true;
    if (seq > g_lfn_seq) {
        g_lfn_seq = seq;
    }
}

/* 把累积的 LFN 写入 out（最长 LFN_CAP-1），返回长度；无 LFN 返回 0 */
static int lfn_pull(char *out)
{
    if (!g_lfn_valid) {
        return 0;
    }
    int end = g_lfn_seq * 13;
    if (end > LFN_CAP - 1) {
        end = LFN_CAP - 1;
    }
    int i = 0;
    while (i < end && g_lfn[i]) {
        out[i] = g_lfn[i];
        i++;
    }
    out[i] = '\0';
    return i;
}

/* 大小写不敏感比较（FAT32 文件名匹配用） */
static int ci_strcmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) {
            return (unsigned char)ca - (unsigned char)cb;
        }
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* 遍历任意目录的簇链：累加 LFN，对每个最终条目调用 cb（返回 true 即停止）。
 * cb 收到的 name 为长名（LFN）或回退 8.3 短名，长度写满 name[LFN_CAP]。 */
static void read_dir(uint32_t start_clus, dir_cb cb, void *priv)
{
    uint8_t sec[SECTOR];
    uint32_t clus = start_clus;
    lfn_reset();
    while (clus >= 2 && clus < 0x0FFFFFF8) {
        for (uint32_t s = 0; s < g_sec_per_clus; s++) {
            if (!disk_read(clus_to_lba(clus) + s, 1, sec)) {
                return;
            }
            for (int off = 0; off < SECTOR; off += 32) {
                const uint8_t *e = sec + off;
                if (e[0] == 0x00) {
                    return;                          /* 目录结束 */
                }
                /* LFN 条目：属性 0x0F，继续累积（不调用 cb） */
                if ((e[11] & 0x0F) == 0x0F && e[0] != 0xE5) {
                    lfn_add(e);
                    continue;
                }
                /* 删除项 / 卷标：复位 LFN 累加后跳过 */
                if (e[0] == 0xE5 || (e[11] & 0x08)) {
                    lfn_reset();
                    continue;
                }
                /* 普通 8.3 条目：组合名字（LFN 优先） */
                char name[LFN_CAP];
                if (lfn_pull(name) == 0) {
                    fmt_83(e, name);
                }
                lfn_reset();
                if (cb(e, name, priv)) {
                    return;
                }
            }
        }
        clus = fat_next(clus);
    }
}

static bool path_find_cb(const uint8_t *e, const char *name, void *priv)
{
    path_cb_t *c = (path_cb_t *)priv;
    if (ci_strcmp(name, c->want) == 0) {
        c->is_dir = (e[11] & 0x10) != 0;
        c->clus = ((uint32_t)rd16(e + 20) << 16) | rd16(e + 26);
        c->size = rd32(e + 28);
        c->found = true;
        return true;
    }
    return false;
}

/* 解析 '/' 分隔的路径，返回最终条目（组件大小写不敏感，匹配 8.3 大写名）。 */
static void resolve_path(const char *path, path_cb_t *out)
{
    char comp[8][13];
    int ncomp = 0;
    const char *p = path;
    while (*p && ncomp < 8) {
        char *c = comp[ncomp];
        int i = 0;
        while (*p && *p != '/') {
            c[i++] = *p++;
        }
        c[i] = '\0';
        upcase_str(c);
        ncomp++;
        if (*p == '/') {
            p++;
        }
    }
    *out = (path_cb_t){ 0 };
    if (ncomp == 0) {
        return;
    }
    uint32_t clus = g_root_clus;
    for (int i = 0; i < ncomp; i++) {
        path_cb_t f = { comp[i], 0, 0, false, false };
        read_dir(clus, path_find_cb, &f);
        if (!f.found) {
            return;
        }
        if (i == ncomp - 1) {
            *out = f;
            return;
        }
        if (!f.is_dir) {
            return;
        }
        clus = f.clus;
    }
}

/* ---- READ ---- */
static uint32_t read_file(uint32_t clus, uint32_t size, char *out, uint32_t cap)
{
    uint8_t sec[SECTOR];
    uint32_t done = 0;
    uint32_t remain = size < cap ? size : cap;
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

/* ---- 带偏移分块读（大文件流式播放，如 playaudio 读 6.8MB MP3） ----
 * 顺序读优化：缓存上次定位到的簇索引与簇号，避免每次从簇链头重新遍历
 * （否则 O(n^2) 遍历会让大文件读取极慢）。 */
static uint32_t g_ra_first = 0, g_ra_idx = 0, g_ra_clus = 0;

/* 预读数据缓存：把连续簇区段一次性预读进 32KB 缓存（内部按
 * DISK_MAX_SECTORS=7 扇区分批发 IPC，受内联消息 3968B 上限约束）。
 * 流式播放每请求仅 FS_DATA_MAX(3500B)，一个 32KB 缓存可命中后续 ~9 次
 * 请求，磁盘 IPC 停顿从每请求一次降为每 9 请求一次，播放连贯。 */
#define RA_SECS  64                            /* 预读窗口：64 扇区 = 32KB */
static uint32_t g_blk_lba  = 0;
static uint32_t g_blk_secs = 0;                /* 0 = 缓存无效 */
static uint8_t  g_blk_buf[RA_SECS * SECTOR];

/* 读取 [lba, lba+secs) 到 dst：按 DISK_MAX_SECTORS 分批发磁盘 IPC。 */
static bool disk_read_batched(uint32_t lba, uint32_t secs, uint8_t *dst)
{
    while (secs > 0) {
        uint32_t c = secs > DISK_MAX_SECTORS ? DISK_MAX_SECTORS : secs;
        if (!disk_read(lba, c, dst)) {
            return false;
        }
        lba  += c;
        dst  += c * SECTOR;
        secs -= c;
    }
    return true;
}

static uint32_t read_file_at(uint32_t first_clus, uint32_t size,
                             uint32_t offset, char *out, uint32_t cap)
{
    if (offset >= size) {
        return 0;                              /* EOF */
    }
    uint32_t clus_bytes = g_sec_per_clus * SECTOR;
    uint32_t want_idx = offset / clus_bytes;

    uint32_t clus, idx;
    if (first_clus == g_ra_first && g_ra_clus >= 2 && g_ra_idx <= want_idx) {
        clus = g_ra_clus;                      /* 命中缓存：从上次位置继续 */
        idx  = g_ra_idx;
    } else {
        clus = first_clus;
        idx  = 0;
    }
    while (idx < want_idx && clus >= 2 && clus < 0x0FFFFFF8) {
        clus = fat_next(clus);
        idx++;
    }
    if (clus < 2 || clus >= 0x0FFFFFF8) {
        return 0;
    }
    g_ra_first = first_clus;
    g_ra_idx   = idx;
    g_ra_clus  = clus;

    uint32_t remain = size - offset;
    if (remain > cap) {
        remain = cap;
    }
    uint32_t within = offset % clus_bytes;     /* 当前簇内字节偏移 */
    uint32_t done = 0;

    while (remain > 0 && clus >= 2 && clus < 0x0FFFFFF8) {
        /* 探测从 clus 起的连续簇（fat 项 = 上一簇+1 即物理连续），
         * 上限为预读窗口 RA_SECS。fat_next 有 FAT 扇区缓存，此探测
         * 几乎不产生磁盘读。 */
        uint32_t run = 1;
        uint32_t probe = clus;
        while (run < RA_SECS / g_sec_per_clus) {
            uint32_t nx = fat_next(probe);
            if (nx != probe + 1 || nx < 2 || nx >= 0x0FFFFFF8) {
                break;
            }
            probe = nx;
            run++;
        }
        uint32_t run_secs = run * g_sec_per_clus;
        uint32_t base_lba = clus_to_lba(clus);
        uint32_t avail    = run_secs * SECTOR - within;   /* run 内可取字节 */
        uint32_t n = remain < avail ? remain : avail;

        /* 本次需要的扇区区间 [need_first, need_last)（绝对 LBA）。 */
        uint32_t need_first = base_lba + within / SECTOR;
        uint32_t need_last  = base_lba + (within + n + SECTOR - 1) / SECTOR;
        if (!(g_blk_secs != 0 && need_first >= g_blk_lba &&
              need_last <= g_blk_lba + g_blk_secs)) {
            /* 缓存未命中：把整个 run（<=RA_SECS 扇区）预读进缓存，
             * 内部按 DISK_MAX_SECTORS 分批 IPC。 */
            if (!disk_read_batched(base_lba, run_secs, g_blk_buf)) {
                return done;
            }
            g_blk_lba  = base_lba;
            g_blk_secs = run_secs;
        }
        u_memcpy(out + done,
                 g_blk_buf + (need_first - g_blk_lba) * SECTOR
                           + within % SECTOR,
                 n);
        done   += n;
        remain -= n;

        /* 按实际消费的字节数推进簇指针（此前按整个 run 推进会令
         * g_ra_idx 超过下次请求的 want_idx，导致顺序读缓存永远失效、
         * 每次请求都从簇链头重新遍历——正是播放断续的根因之一）。 */
        uint32_t adv = (within + n) / clus_bytes;         /* 消费的完整簇数 */
        within = (within + n) % clus_bytes;
        for (uint32_t i = 0; i < adv; i++) {
            if (clus < 2 || clus >= 0x0FFFFFF8) {
                break;
            }
            clus = fat_next(clus);
            g_ra_idx++;
        }
        g_ra_clus = clus;
    }
    return done;
}

/* ---- 服务循环 ---- */
static uint8_t g_req[512];
static uint8_t g_resp[sizeof(mach_msg_header_t) + sizeof(fs_resp_t) + FS_DATA_MAX];

/* execve 读文件用的 OOL 发送缓冲（页对齐，最多 16 页 = 64KiB）。
 * 布局：[fs_resp_t][文件内容...]，内核侧把该 OOL 直接拷入内核缓冲后
 * 先解析 fs_resp，再加载后续 ELF 字节（符合内核 [header][ool_desc] 协议）。 */
static uint8_t g_filebuf[16 * 4096] __attribute__((aligned(4096)));

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
            path_cb_t f;
            resolve_path(name, &f);
            if (!f.found) {
                fr->status = FS_ERR_NOENT;
            } else {
                fr->length = read_file(f.clus, f.size, data, FS_DATA_MAX);
            }
        } else if (rh->msgh_id == FS_MSG_READ_AT) {
            /* 分块读：负载 = fs_read_at_req_t + 文件名 */
            fs_read_at_req_t *ra = (fs_read_at_req_t *)(g_req + sizeof(*rh));
            char *name = (char *)(g_req + sizeof(*rh) + sizeof(*ra));
            g_req[sizeof(g_req) - 1] = 0;
            uint32_t want = ra->length;
            if (want > FS_DATA_MAX) {
                want = FS_DATA_MAX;
            }
            path_cb_t f;
            resolve_path(name, &f);
            if (!f.found || f.is_dir) {
                fr->status = FS_ERR_NOENT;
            } else {
                fr->length = read_file_at(f.clus, f.size, ra->offset,
                                          data, want);
            }
        } else if (rh->msgh_id == FS_MSG_READ_FILE) {
            /* 内核 execve 专用：把完整文件内容经 OOL 回传。
             * OOL 缓冲布局 = [fs_resp_t][文件内容...]（无内联 fs_resp）。 */
            char *name = (char *)g_req + sizeof(*rh);
            g_req[sizeof(g_req) - 1] = 0;
            path_cb_t f;
            resolve_path(name, &f);
            if (!f.found) {
                fr->status = FS_ERR_NOENT;
                fr->length = 0;
                uint32_t total = sizeof(*h) + sizeof(*fr);
                h->msgh_bits = 0;
                h->msgh_size = total;
                h->msgh_remote_port = reply;
                h->msgh_local_port = FS_PORT;
                h->msgh_id = rh->msgh_id;
                h->msgh_reserved = 0;
                mach_msg_send(g_resp, total);
            } else {
                uint32_t len = read_file(f.clus, f.size,
                                         (char *)g_filebuf + sizeof(fs_resp_t),
                                         16 * 4096 - sizeof(fs_resp_t));
                fs_resp_t *ofr = (fs_resp_t *)g_filebuf;
                ofr->status = FS_OK;
                ofr->length = len;
                h->msgh_bits = MACH_MSGH_BITS_OOL;
                /* 关键：内联部分必须包含 OOL 描述符，否则内核 sys_mach_msg
                 * 会以 MACH_INVALID_ARGUMENT 拒收（msgh_size < 头+描述符）。 */
                h->msgh_size = sizeof(*h) + sizeof(ool_desc_t);
                h->msgh_remote_port = reply;
                h->msgh_local_port = FS_PORT;
                h->msgh_id = rh->msgh_id;
                h->msgh_reserved = 0;
                ool_desc_t *d = (ool_desc_t *)(g_resp + sizeof(*h));
                d->address = (uint64_t)g_filebuf;
                d->size = (sizeof(fs_resp_t) + len + PAGE_SIZE - 1)
                          & ~((uint64_t)PAGE_SIZE - 1);
                if (d->size == 0) {
                    d->size = PAGE_SIZE;
                }
                mach_msg_send(g_resp, sizeof(*h) + sizeof(ool_desc_t));
            }
            continue;                           /* 已自行应答，跳过底部通用内联应答 */
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

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
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
