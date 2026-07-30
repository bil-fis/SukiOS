/*
 * user/fs_server.c
 * -----------------------------------------------------------------------------
 * FS_SERVER：Ring3 FAT32 文件系统服务（读/写，P1-2 写路径完整化）。
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
/* M13 修复：单条簇链最大遍历步数（远超任何实际 FAT32 卷的簇数），用于
 * 在 read_dir/read_file/read_file_at 中给簇链跟随循环兜底，拦截环簇链死循环。 */
#define FAT_WALK_LIMIT  0x400000U

/* ---- 与内核 disk-srv 的通信 ---- */
static uint8_t g_diskbuf[sizeof(mach_msg_header_t) + sizeof(disk_read_resp_t)
                         + DISK_MAX_SECTORS * SECTOR];

static bool disk_read(uint32_t lba, uint32_t count, void *out)
{
    /* M16 修复：拒绝越界/非法扇区数拷贝，防止后续 memcpy 越出调用方缓冲。 */
    if (count == 0 || count > DISK_MAX_SECTORS) {
        return false;
    }
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
/* M13/M14 修复：卷总簇数，用于校验簇号落于数据区有效范围，并作为簇链遍历
 * 步数上限的依据，防止损坏文件系统的环簇链导致无限循环/读飞。 */
static uint32_t g_total_clusters = 0;

/* P1-2 写路径：FAT 副本数与每 FAT 扇区数（mount 时采集），用于写回全部
 * FAT 副本；g_fat_scan 为下次簇分配扫描起点（加速连续分配）。 */
static uint8_t  g_nfats = 1;
static uint32_t g_fat_size = 0;
static uint32_t g_fat_scan = 2;
/* 整簇清零缓冲（FAT32 单簇最多 128 扇区 = 64KB），用于新目录/扩展簇清零。 */
static uint8_t  g_zero[128 * SECTOR];

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
    g_nfats               = nfats ? nfats : 1;   /* P1-2：写回全部 FAT 副本 */
    g_fat_size            = fatsz32;

    if (byts_per_sec != SECTOR || g_sec_per_clus == 0 || fatsz32 == 0) {
        return false;
    }
    g_fat_begin  = rsvd;
    g_data_begin = rsvd + nfats * fatsz32;

    /* M13/M14：计算总簇数，供簇号范围校验与遍历步数上限。 */
    uint32_t tot_sec = rd32(bpb + 32);
    if (tot_sec == 0) {
        tot_sec = rd16(bpb + 19);
    }
    if (tot_sec > g_data_begin && g_sec_per_clus != 0) {
        g_total_clusters = (tot_sec - g_data_begin) / g_sec_per_clus;
    } else {
        g_total_clusters = 0;
    }

    char n[24];
    u_print("[fs] FAT32 mounted: spc=");
    u_print(u_utoa_s(g_sec_per_clus, n, sizeof(n)));
    u_print(" fat@");
    u_print(u_utoa_s(g_fat_begin, n, sizeof(n)));
    u_print(" data@");
    u_print(u_utoa_s(g_data_begin, n, sizeof(n)));
    u_print(" root_clus=");
    u_print(u_utoa_s(g_root_clus, n, sizeof(n)));
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
        u_utoa_s(size, num, sizeof(num));
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
static int    g_lfn_prev;    /* 上一条已处理的序列号（L7 连续性校验用） */
static bool   g_lfn_valid;
static bool   g_lfn_broken;  /* L7：链中出现序号断层/伪造时整条作废 */

static void lfn_reset(void)
{
    /* L7 修复：整段缓冲清零（而非仅 g_lfn[0]='\0'），避免上一条 LFN 残留的
     * 非零字节在断层被判无效后，仍被 lfn_pull 的"遇 NUL 即止"逻辑误纳入新名。 */
    memset(g_lfn, 0, sizeof(g_lfn));
    g_lfn_valid = false;
    g_lfn_broken = false;
    g_lfn_seq = 0;
    g_lfn_prev = 0;
}

/* 解码一条 LFN 条目，写入 (seq-1)*13 处（UTF-16LE 的 ASCII 部分） */
static void lfn_add(const uint8_t *e)
{
    if (g_lfn_broken) {                    /* 已判定无效：忽略后续条目 */
        return;
    }
    int seq = e[0] & 0x1F;                  /* 低 5 位 = 序列号(1-based) */
    if (seq == 0 || seq * 13 >= LFN_CAP) { /* 越界/非法序号直接作废 */
        g_lfn_broken = true;
        return;
    }
    /* L7 修复：LFN 物理逆序排列，首条为最高序号。要求每条序号严格递减 1，
     * 任何断层（序号被伪造跳变、或重复）都说明链不可信，整条作废，
     * 回退到 8.3 短名，杜绝"截断处无 NUL / 注入垃圾字符"问题。 */
    if (g_lfn_seq == 0) {
        g_lfn_prev = seq;                   /* 首条：记录起点序号 */
    } else if (seq != g_lfn_prev - 1) {
        g_lfn_broken = true;
        return;
    } else {
        g_lfn_prev = seq;
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

/* 把累积的 LFN 写入 out（最长 LFN_CAP-1），返回长度；无 LFN 或链已作废返回 0 */
static int lfn_pull(char *out)
{
    if (!g_lfn_valid || g_lfn_broken) {     /* L7：断层链回退 8.3 短名 */
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
    uint32_t hops = 0;
    lfn_reset();
    while (clus >= 2 && clus < 0x0FFFFFF8 && hops++ < FAT_WALK_LIMIT) {
        /* M14 修复：簇号须落在数据区有效范围 [2, 2+g_total_clusters)。
         * 越界簇号（损坏 FS）会令 clus_to_lba 算出非法 LBA 误读任意扇区。 */
        if (g_total_clusters != 0 && clus - 2 >= g_total_clusters) {
            break;
        }
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
        /* M15 修复：组件长度上限 12（留 1 字节 NUL），超长非法 8.3 名不再
         * 静默截断后误匹配。 */
        while (*p && *p != '/' && i < 12) {
            c[i++] = *p++;
        }
        c[i] = '\0';
        if (i == 12 && *p && *p != '/') {     /* 组件超长 → 必然不是 8.3 名 */
            *out = (path_cb_t){ 0 };
            return;
        }
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
    /* M15 修复：路径深度超过 8 级时显式返回未找到，而非静默丢弃深层组件
     * 后拿前 8 级去匹配（曾可能命中错误文件）。 */
    if (*p != 0) {
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
    uint32_t hops_rf = 0;
    while (remain > 0 && clus >= 2 && clus < 0x0FFFFFF8
           && hops_rf++ < FAT_WALK_LIMIT) {
        if (g_total_clusters != 0 && clus - 2 >= g_total_clusters) {
            break;
        }
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
    if (g_total_clusters != 0 && clus - 2 >= g_total_clusters) {
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

    uint32_t hops_rfa = 0;
    while (remain > 0 && clus >= 2 && clus < 0x0FFFFFF8
           && hops_rfa++ < FAT_WALK_LIMIT) {
        if (g_total_clusters != 0 && clus - 2 >= g_total_clusters) {
            break;
        }
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

/* =========================================================================
 * P1-2：FAT32 写路径
 *
 * 设计要点（生产稳定性红线）：
 *  - 所有簇号/链长/扇区数均做越界与环链保护（g_total_clusters 校验 + FAT_WALK_LIMIT）。
 *  - FAT 表项写回全部 FAT 副本（g_nfats），写后立即使读缓存失效避免读到陈旧项。
 *  - 目录条目写入保证连续无空洞，read_dir 的“遇 0x00 即止”语义可被正确穿越簇链读取。
 *  - 扩展目录/文件时新分配的簇先整簇清零，保证稀疏区读回为 0。
 *  - 所有磁盘写经 disk_write -> 内核 DISK_MSG_WRITE -> blk_write（ATA PIO / AHCI DMA）。
 * ========================================================================= */

/* 写请求缓冲（含 7 扇区数据上限，与内核 DISK_MAX_SECTORS 一致）。 */
static uint8_t g_wreq[sizeof(mach_msg_header_t) + sizeof(disk_write_req_t)
                      + DISK_MAX_SECTORS * SECTOR];

static bool disk_write(uint64_t lba, uint8_t count, const void *buf)
{
    if (count == 0 || count > DISK_MAX_SECTORS) {
        return false;
    }
    mach_msg_header_t *h   = (mach_msg_header_t *)g_wreq;
    disk_write_req_t  *req = (disk_write_req_t *)(g_wreq + sizeof(mach_msg_header_t));
    req->lba = lba;
    req->count = count;
    req->pad = 0;
    u_memcpy(g_wreq + sizeof(mach_msg_header_t) + sizeof(disk_write_req_t),
             buf, (uint32_t)count * SECTOR);
    h->msgh_bits = 0;
    h->msgh_size = sizeof(mach_msg_header_t) + sizeof(disk_write_req_t)
                   + (uint32_t)count * SECTOR;
    h->msgh_remote_port = DISK_PORT;
    h->msgh_local_port  = FS_REPLY_PORT;
    h->msgh_id = DISK_MSG_WRITE;
    h->msgh_reserved = 0;
    if (mach_msg_send(g_wreq, h->msgh_size) != MACH_MSG_SUCCESS) {
        return false;
    }
    if (mach_msg_recv(g_diskbuf, sizeof(g_diskbuf), FS_REPLY_PORT) != MACH_MSG_SUCCESS) {
        return false;
    }
    disk_write_resp_t *rr = (disk_write_resp_t *)(g_diskbuf + sizeof(mach_msg_header_t));
    return rr->status == 0;
}

/* ---- FAT 写回 ---- */
static bool fat_set(uint32_t clus, uint32_t val)
{
    if (clus < 2 || g_total_clusters == 0 || clus - 2 >= g_total_clusters) {
        return false;                       /* 越界簇号拒绝写入 */
    }
    val &= 0x0FFFFFFF;
    uint32_t off = clus * 4;
    uint32_t sec_off = off / SECTOR;
    uint32_t byte_off = off % SECTOR;
    uint8_t sec[SECTOR];
    for (uint8_t i = 0; i < g_nfats; i++) {
        uint32_t lba = g_fat_begin + (uint64_t)i * g_fat_size + sec_off;
        if (!disk_read(lba, 1, sec)) {
            return false;
        }
        sec[byte_off]     = (uint8_t)(val & 0xFF);
        sec[byte_off + 1] = (uint8_t)((val >> 8) & 0xFF);
        sec[byte_off + 2] = (uint8_t)((val >> 16) & 0xFF);
        sec[byte_off + 3] = (uint8_t)((val >> 24) & 0xFF);
        if (!disk_write(lba, 1, sec)) {
            return false;
        }
    }
    g_fat_cache_ok = false;                 /* 读缓存失效，下次 fat_next 重读 */
    return true;
}

static uint32_t fat_alloc_cluster(void)
{
    if (g_total_clusters == 0) {
        return 0;
    }
    uint32_t base = (g_fat_scan < 2) ? 2 : g_fat_scan;
    for (uint32_t i = 0; i < g_total_clusters; i++) {
        uint32_t c = 2 + ((base - 2 + i) % g_total_clusters);
        if (fat_next(c) == 0) {              /* 空闲簇 */
            if (!fat_set(c, 0x0FFFFFFF)) {
                return 0;
            }
            g_fat_scan = c + 1;
            if (g_fat_scan - 2 >= g_total_clusters) {
                g_fat_scan = 2;
            }
            return c;
        }
    }
    u_print("[fs] no free clusters\n");
    return 0;
}

static bool fat_free_chain(uint32_t first)
{
    uint32_t c = first;
    uint32_t total = g_total_clusters ? g_total_clusters : 1;
    uint32_t guard = 0;
    while (c >= 2 && c - 2 < total && guard++ <= total + 1) {
        uint32_t nx = fat_next(c);
        if (!fat_set(c, 0)) {                /* 标记空闲 */
            return false;
        }
        if (nx >= 2 && nx - 2 < total) {
            c = nx;                           /* 合法下一簇：继续 */
        } else {
            break;                           /* EOC / 非法：链结束 */
        }
    }
    return true;
}

/* 整簇清零（新目录首簇 / 文件扩展簇，保证稀疏区读回为 0）。 */
static bool write_zero_cluster(uint32_t clus)
{
    uint32_t nsec = g_sec_per_clus;
    if (nsec > 128) nsec = 128;
    for (uint32_t s = 0; s < nsec; s++) {
        if (!disk_write(clus_to_lba(clus) + s, 1, g_zero)) {
            return false;
        }
    }
    return true;
}

/* ---- 目录条目写 ---- */
static bool dir_patch(uint32_t lba, uint32_t off, const uint8_t *src, uint32_t len)
{
    if (off > SECTOR || off + len > SECTOR) {
        return false;
    }
    uint8_t sec[SECTOR];
    if (!disk_read(lba, 1, sec)) {
        return false;
    }
    u_memcpy(sec + off, src, len);
    return disk_write(lba, 1, sec);
}

static bool write_entry_at(uint32_t lba, uint32_t off, const uint8_t e[32])
{
    return dir_patch(lba, off, e, 32);
}

/* 把全局目录项序号 idx 映射到 (簇, 簇内字节偏移)。 */
static bool dir_entry_pos(uint32_t start_clus, uint32_t idx,
                          uint32_t *out_clus, uint32_t *out_byte)
{
    uint32_t entries_per_clus = g_sec_per_clus * 16;
    uint32_t clus = start_clus;
    uint32_t skip = idx;
    uint32_t hops = 0;
    while (skip >= entries_per_clus && clus >= 2 && clus < 0x0FFFFFF8
           && hops++ < FAT_WALK_LIMIT) {
        if (g_total_clusters && clus - 2 >= g_total_clusters) {
            return false;
        }
        clus = fat_next(clus);
        skip -= entries_per_clus;
    }
    if (clus < 2 || clus >= 0x0FFFFFF8) {
        return false;
    }
    *out_clus = clus;
    *out_byte = skip * 32;
    return true;
}

/* 在目录中查找 count 个连续空闲项（0x00 或 0xE5），返回首个项的全局序号。
 * 若目录链结束仍不足，则扩展一个新簇（整簇清零后链入）继续。 */
static bool dir_find_free_run(uint32_t start_clus, uint32_t count, uint32_t *out_index)
{
    if (count == 0) {
        return false;
    }
    uint32_t clus = start_clus;
    uint32_t global = 0;
    uint32_t hops = 0;
    uint32_t run = 0, run_start = 0;
    uint8_t sec[SECTOR];
    while (clus >= 2 && clus < 0x0FFFFFF8 && hops++ < FAT_WALK_LIMIT) {
        if (g_total_clusters && clus - 2 >= g_total_clusters) {
            break;
        }
        for (uint32_t s = 0; s < g_sec_per_clus; s++) {
            uint32_t lba = clus_to_lba(clus) + s;
            if (!disk_read(lba, 1, sec)) {
                return false;
            }
            for (uint32_t off = 0; off < SECTOR; off += 32) {
                uint8_t b = sec[off];
                bool free_e = (b == 0x00 || b == 0xE5);
                if (free_e) {
                    if (run == 0) {
                        run_start = global;
                    }
                    run++;
                    if (run >= count) {
                        *out_index = run_start;
                        return true;
                    }
                } else {
                    run = 0;
                }
                global++;
            }
        }
        uint32_t nxt = fat_next(clus);
        if (nxt >= 2 && nxt < 0x0FFFFFF8) {
            clus = nxt;
        } else {
            /* 目录链结束 -> 扩展新簇 */
            uint32_t nc = fat_alloc_cluster();
            if (nc == 0) {
                return false;
            }
            if (!write_zero_cluster(nc)) {
                /* 仍尽量链接，后续写会覆盖 */
            }
            if (!fat_set(clus, nc)) {
                return false;
            }
            clus = nc;
        }
    }
    return false;
}

/* ---- 名称 / 8.3 / LFN ---- */
static uint8_t to_up(uint8_t c)
{
    if (c >= 'a' && c <= 'z') return (uint8_t)(c - 32);
    return c;
}
static uint8_t fix_char(uint8_t c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    switch (c) {
        case '$': case '%': case '\'': case '-': case '_':
        case '@': case '~': case '`': case '!': case '(':
        case ')': case '{': case '}': case '^': case '#':
        case '&': case '+': case ',': case '.': case ';':
        case '=': case '[': case ']': return c;
        default: return '_';
    }
}

static void make_shortname(const char *in, uint8_t out83[11])
{
    for (int i = 0; i < 11; i++) out83[i] = ' ';
    const char *p = in;
    while (*p == '/' || *p == '\\') p++;
    const char *dot = NULL;
    for (const char *q = p; *q; q++) if (*q == '.') dot = q;
    if (dot == p) dot = NULL;               /* 以 '.' 开头的文件不切扩展名 */
    char base[9]; for (int i = 0; i < 8; i++) base[i] = ' ';
    char ext[4];  for (int i = 0; i < 3; i++) ext[i] = ' ';
    if (dot) {
        uint32_t bn = 0;
        for (const char *s = p; s < dot && bn < 8; s++)
            base[bn++] = (char)to_up(fix_char((uint8_t)*s));
        uint32_t en = 0;
        for (const char *s = dot + 1; *s && en < 3; s++)
            ext[en++] = (char)to_up(fix_char((uint8_t)*s));
    } else {
        uint32_t bn = 0;
        for (const char *s = p; *s && bn < 8; s++)
            base[bn++] = (char)to_up(fix_char((uint8_t)*s));
    }
    for (int i = 0; i < 8; i++) out83[i] = (uint8_t)base[i];
    for (int i = 0; i < 3; i++) out83[8 + i] = (uint8_t)ext[i];
}

static bool dir_has_83(uint32_t start_clus, const uint8_t *s83)
{
    uint8_t sec[SECTOR];
    uint32_t clus = start_clus;
    uint32_t hops = 0;
    while (clus >= 2 && clus < 0x0FFFFFF8 && hops++ < FAT_WALK_LIMIT) {
        if (g_total_clusters && clus - 2 >= g_total_clusters) break;
        for (uint32_t s = 0; s < g_sec_per_clus; s++) {
            if (!disk_read(clus_to_lba(clus) + s, 1, sec)) return false;
            for (uint32_t off = 0; off < SECTOR; off += 32) {
                const uint8_t *e = sec + off;
                if (e[0] == 0x00) return false;            /* 目录结束 */
                if ((e[11] & 0x0F) == 0x0F) continue;       /* LFN */
                if (e[0] == 0xE5) continue;                 /* 已删 */
                if (e[11] & 0x08) continue;                 /* 卷标 */
                bool eq = true;
                for (int i = 0; i < 11; i++) {
                    uint8_t a = s83[i], b = e[i];
                    if (a >= 'a' && a <= 'z') a -= 32;
                    if (b >= 'a' && b <= 'z') b -= 32;
                    if (a != b) { eq = false; break; }
                }
                if (eq) return true;
            }
        }
        clus = fat_next(clus);
    }
    return false;
}

/* 生成唯一 8.3 名：若与现有冲突，追加 '~' + 数字（仿 Windows 风格）。 */
static bool make_unique_83(uint32_t parent_clus, const char *base, uint8_t out83[11])
{
    make_shortname(base, out83);
    int attempts = 0;
    while (dir_has_83(parent_clus, out83)) {
        uint8_t n83[11]; for (int i = 0; i < 11; i++) n83[i] = ' ';
        for (int i = 0; i < 6 && i < 8 && out83[i] != ' '; i++) n83[i] = out83[i];
        n83[6] = '~';
        n83[7] = (uint8_t)('1' + (attempts % 9));
        for (int i = 8; i < 11; i++) n83[i] = out83[i];
        for (int i = 0; i < 11; i++) out83[i] = n83[i];
        if (++attempts > 99) return false;
    }
    return true;
}

static void lfn_checksum(const uint8_t *s83, uint8_t *out)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        uint8_t c = s83[i];
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + c);
    }
    *out = sum;
}

static void build_83_entry(uint8_t e[32], const uint8_t *s83, uint8_t attr,
                           uint32_t clus, uint32_t size)
{
    for (int i = 0; i < 32; i++) e[i] = 0;
    for (int i = 0; i < 11; i++) e[i] = s83[i];
    e[11] = attr;
    e[12] = 0;
    e[13] = 0;
    /* 创建/写日期：0x21 = 1980-01-01，时间 0（合法 FAT 日期） */
    e[16] = 0x21; e[17] = 0;
    e[18] = 0x21; e[19] = 0;
    e[20] = (uint8_t)((clus >> 16) & 0xFF);
    e[21] = (uint8_t)((clus >> 24) & 0xFF);
    e[22] = 0; e[23] = 0;
    e[24] = 0x21; e[25] = 0;
    e[26] = (uint8_t)(clus & 0xFF);
    e[27] = (uint8_t)((clus >> 8) & 0xFF);
    e[28] = (uint8_t)(size & 0xFF);
    e[29] = (uint8_t)((size >> 8) & 0xFF);
    e[30] = (uint8_t)((size >> 16) & 0xFF);
    e[31] = (uint8_t)((size >> 24) & 0xFF);
}

static void build_lfn_entry(uint32_t seq, uint32_t nlfn, uint8_t sum,
                            const char *name, uint8_t e[32])
{
    for (int i = 0; i < 32; i++) e[i] = 0;
    e[0] = (seq == nlfn) ? (uint8_t)(0x40 | seq) : (uint8_t)seq;
    e[11] = 0x0F;
    e[12] = 0;
    e[13] = sum;
    e[26] = 0;
    e[27] = 0;
    int nchars = (int)u_strlen(name);
    for (int k = 0; k < 13; k++) {
        int idx = (int)(seq - 1) * 13 + k;
        uint16_t w;
        if (idx < nchars) w = (uint16_t)(unsigned char)name[idx];
        else if (idx == nchars) w = 0x0000;  /* 长名结束符 */
        else w = 0xFFFF;                      /* 填充 */
        uint8_t *dst;
        if (k < 5) dst = e + 1 + 2 * k;
        else if (k < 11) dst = e + 14 + 2 * (k - 5);
        else dst = e + 28 + 2 * (k - 11);
        dst[0] = (uint8_t)(w & 0xFF);
        dst[1] = (uint8_t)((w >> 8) & 0xFF);
    }
}

typedef struct { uint32_t lba; uint32_t off; } slot_t;

typedef struct {
    bool     found;
    slot_t   entry;                  /* 8.3 条目位置 */
    int      nlfn;
    slot_t   lfn[32];                /* 其前导 LFN 条目位置（物理逆序） */
    uint32_t first_clus;
    uint32_t size;
    bool     is_dir;
} dir_loc_t;

/* 在目录中按名查找条目，返回 8.3 条目位置、前导 LFN 位置、首簇、大小、属性。 */
static void dir_lookup(uint32_t start_clus, const char *want, dir_loc_t *out)
{
    *out = (dir_loc_t){ 0 };
    uint8_t sec[SECTOR];
    uint32_t clus = start_clus;
    uint32_t hops = 0;
    lfn_reset();
    slot_t lfn_slots[32];
    int nlfn = 0;
    while (clus >= 2 && clus < 0x0FFFFFF8 && hops++ < FAT_WALK_LIMIT) {
        if (g_total_clusters && clus - 2 >= g_total_clusters) break;
        for (uint32_t s = 0; s < g_sec_per_clus; s++) {
            uint32_t lba = clus_to_lba(clus) + s;
            if (!disk_read(lba, 1, sec)) return;
            for (uint32_t off = 0; off < SECTOR; off += 32) {
                const uint8_t *e = sec + off;
                if (e[0] == 0x00) return;                 /* 目录结束 */
                if ((e[11] & 0x0F) == 0x0F && e[0] != 0xE5) {
                    if (nlfn < 32) {
                        lfn_slots[nlfn].lba = lba;
                        lfn_slots[nlfn].off = off;
                        nlfn++;
                    }
                    lfn_add(e);
                    continue;
                }
                if (e[0] == 0xE5 || (e[11] & 0x08)) {
                    lfn_reset(); nlfn = 0;
                    continue;
                }
                char name[LFN_CAP];
                if (lfn_pull(name) == 0) fmt_83(e, name);
                lfn_reset();
                if (ci_strcmp(name, want) == 0) {
                    out->found = true;
                    out->entry.lba = lba;
                    out->entry.off = off;
                    out->nlfn = nlfn;
                    for (int i = 0; i < nlfn; i++) out->lfn[i] = lfn_slots[i];
                    out->first_clus = ((uint32_t)rd16(e + 20) << 16) | rd16(e + 26);
                    out->size = rd32(e + 28);
                    out->is_dir = (e[11] & 0x10) != 0;
                    return;
                }
            }
        }
        clus = fat_next(clus);
    }
}

static bool dir_empty_cb(const uint8_t *e, const char *name, void *priv)
{
    (void)e;
    bool *empty = (bool *)priv;
    if (ci_strcmp(name, ".") == 0 || ci_strcmp(name, "..") == 0) return false;
    *empty = false;
    return true;                                /* 发现非 . / .. 条目 -> 非空 */
}
static bool dir_is_empty(uint32_t clus)
{
    bool empty = true;
    read_dir(clus, dir_empty_cb, &empty);
    return empty;
}

/* 拆分路径为父目录簇号 + 基名（base 由调用方缓冲承载）。 */
static bool split_parent(const char *path, uint32_t *parent_clus,
                         char *base, uint32_t basemax)
{
    const char *last = NULL;
    for (const char *p = path; *p; p++) if (*p == '/') last = p;
    *parent_clus = g_root_clus;
    if (last) {
        uint32_t n = (uint32_t)(last - path);
        if (n >= basemax) return false;
        char pbuf[64];
        uint32_t m = n < sizeof(pbuf) ? n : (uint32_t)(sizeof(pbuf) - 1);
        for (uint32_t i = 0; i < m; i++) pbuf[i] = path[i];
        pbuf[m] = 0;
        path_cb_t pc; resolve_path(pbuf, &pc);
        if (!pc.found || !pc.is_dir) return false;
        *parent_clus = pc.clus;
        const char *b = last + 1;
        uint32_t i = 0;
        while (*b && i + 1 < basemax) base[i++] = *b++;
        base[i] = 0;
    } else {
        uint32_t i = 0;
        while (*path && i + 1 < basemax) base[i++] = *path++;
        base[i] = 0;
    }
    return base[0] != 0;
}

static uint32_t chain_len(uint32_t first)
{
    uint32_t c = first, n = 0, guard = 0;
    uint32_t total = g_total_clusters ? g_total_clusters : 1;
    while (c >= 2 && c - 2 < total && guard++ <= total + 1) {
        n++; c = fat_next(c);
    }
    return n;
}

static uint32_t cluster_at_index(uint32_t first, uint32_t ci)
{
    uint32_t c = first, i = 0, guard = 0;
    uint32_t total = g_total_clusters ? g_total_clusters : 1;
    while (i < ci && c >= 2 && c - 2 < total && guard++ <= total + 1) {
        c = fat_next(c); i++;
    }
    return c;
}

static bool patch_dir_firstclus(dir_loc_t *loc, uint32_t clus)
{
    uint8_t b[2];
    b[0] = (uint8_t)((clus >> 16) & 0xFF);
    b[1] = (uint8_t)((clus >> 24) & 0xFF);
    if (!dir_patch(loc->entry.lba, loc->entry.off + 20, b, 2)) return false;
    b[0] = (uint8_t)(clus & 0xFF);
    b[1] = (uint8_t)((clus >> 8) & 0xFF);
    return dir_patch(loc->entry.lba, loc->entry.off + 26, b, 2);
}

static bool patch_dir_size(dir_loc_t *loc, uint32_t size)
{
    uint8_t b[4];
    b[0] = (uint8_t)(size & 0xFF);
    b[1] = (uint8_t)((size >> 8) & 0xFF);
    b[2] = (uint8_t)((size >> 16) & 0xFF);
    b[3] = (uint8_t)((size >> 24) & 0xFF);
    return dir_patch(loc->entry.lba, loc->entry.off + 28, b, 4);
}

/* 初始化一个新建目录的首簇（写入 "." 与 ".."）。 */
static bool init_dir_cluster(uint32_t first, uint32_t parent_clus)
{
    if (!write_zero_cluster(first)) return false;
    uint32_t lba = clus_to_lba(first);
    uint8_t e[32];
    for (int i = 0; i < 32; i++) e[i] = 0;
    e[0] = '.'; for (int i = 1; i < 11; i++) e[i] = ' ';
    e[11] = 0x10; e[16] = 0x21; e[18] = 0x21; e[24] = 0x21;
    e[20] = (uint8_t)((first >> 16) & 0xFF); e[21] = (uint8_t)((first >> 24) & 0xFF);
    e[26] = (uint8_t)(first & 0xFF); e[27] = (uint8_t)((first >> 8) & 0xFF);
    if (!dir_patch(lba, 0, e, 32)) return false;
    for (int i = 0; i < 32; i++) e[i] = 0;
    e[0] = '.'; e[1] = '.'; for (int i = 2; i < 11; i++) e[i] = ' ';
    e[11] = 0x10; e[16] = 0x21; e[18] = 0x21; e[24] = 0x21;
    e[20] = (uint8_t)((parent_clus >> 16) & 0xFF); e[21] = (uint8_t)((parent_clus >> 24) & 0xFF);
    e[26] = (uint8_t)(parent_clus & 0xFF); e[27] = (uint8_t)((parent_clus >> 8) & 0xFF);
    return dir_patch(lba, 32, e, 32);
}

/* 创建空文件(is_dir=false)或目录(is_dir=true)。已存在且类型一致则幂等返回 OK。 */
static int fs_create(const char *path, bool is_dir)
{
    uint32_t parent;
    char base[64];
    if (!split_parent(path, &parent, base, sizeof(base))) {
        return FS_ERR_NOENT;
    }
    dir_loc_t loc;
    dir_lookup(parent, base, &loc);
    if (loc.found) {
        if (loc.is_dir == is_dir) return FS_OK;   /* 幂等 */
        return FS_ERR_IO;
    }
    uint32_t first_clus = 0;
    if (is_dir) {
        first_clus = fat_alloc_cluster();
        if (first_clus == 0) return FS_ERR_IO;
        uint32_t parent_for_dot = (parent == g_root_clus) ? 0 : parent;
        if (!init_dir_cluster(first_clus, parent_for_dot)) return FS_ERR_IO;
    }
    uint8_t s83[11];
    if (!make_unique_83(parent, base, s83)) return FS_ERR_IO;
    uint8_t sum;
    lfn_checksum(s83, &sum);
    uint32_t nchars = (uint32_t)u_strlen(base);
    uint32_t nlfn = (nchars + 12) / 13;
    uint32_t count = nlfn + 1;
    uint32_t xidx;
    if (!dir_find_free_run(parent, count, &xidx)) return FS_ERR_IO;
    for (uint32_t i = 0; i < nlfn; i++) {
        uint32_t seq = nlfn - i;                  /* 物理逆序：最高序号先写 */
        uint8_t le[32];
        build_lfn_entry(seq, nlfn, sum, base, le);
        uint32_t c, bo;
        if (!dir_entry_pos(parent, xidx + i, &c, &bo)) return FS_ERR_IO;
        if (!write_entry_at(clus_to_lba(c) + bo / SECTOR, bo % SECTOR, le)) return FS_ERR_IO;
    }
    uint8_t e83[32];
    build_83_entry(e83, s83, is_dir ? (uint8_t)0x10 : (uint8_t)0x20, first_clus, 0);
    {
        uint32_t c, bo;
        if (!dir_entry_pos(parent, xidx + nlfn, &c, &bo)) return FS_ERR_IO;
        if (!write_entry_at(clus_to_lba(c) + bo / SECTOR, bo % SECTOR, e83)) return FS_ERR_IO;
    }
    return FS_OK;
}

static int fs_write(const char *path, const uint8_t *data, uint32_t len, uint32_t offset)
{
    if (len == 0) return FS_OK;
    if (len > FS_WRITE_MAX) len = FS_WRITE_MAX;
    uint32_t parent;
    char base[64];
    if (!split_parent(path, &parent, base, sizeof(base))) return FS_ERR_NOENT;
    dir_loc_t loc;
    dir_lookup(parent, base, &loc);
    if (!loc.found) {
        if (fs_create(path, false) != FS_OK) return FS_ERR_IO;
        dir_lookup(parent, base, &loc);
        if (!loc.found) return FS_ERR_IO;
    }
    uint32_t clus_bytes = g_sec_per_clus * SECTOR;
    uint32_t old_size = loc.size;
    uint32_t start_clus = loc.first_clus;
    uint32_t need = (offset + len + clus_bytes - 1) / clus_bytes;
    uint32_t cur = (start_clus == 0) ? 0 : chain_len(start_clus);
    uint32_t last = start_clus;
    if (cur == 0 && need > 0) {
        uint32_t nc = fat_alloc_cluster();
        if (nc == 0) return FS_ERR_IO;
        if (!write_zero_cluster(nc)) return FS_ERR_IO;
        start_clus = nc; last = nc; cur = 1;
        if (!patch_dir_firstclus(&loc, nc)) return FS_ERR_IO;
    }
    while (cur < need) {
        uint32_t nc = fat_alloc_cluster();
        if (nc == 0) return FS_ERR_IO;
        if (!write_zero_cluster(nc)) return FS_ERR_IO;
        if (!fat_set(last, nc)) return FS_ERR_IO;        /* 链接 */
        if (!fat_set(nc, 0x0FFFFFFF)) return FS_ERR_IO;  /* 新末端标记 EOF */
        last = nc; cur++;
    }
    uint8_t sec[SECTOR];
    uint32_t p = offset;
    uint32_t endp = offset + len;
    while (p < endp) {
        uint32_t ci = p / clus_bytes;
        uint32_t cluster = cluster_at_index(start_clus, ci);
        if (cluster < 2) return FS_ERR_IO;
        uint32_t within = p % clus_bytes;
        uint32_t si = within / SECTOR;
        uint32_t bo = within % SECTOR;
        uint32_t lba = clus_to_lba(cluster) + si;
        if (!disk_read(lba, 1, sec)) return FS_ERR_IO;
        uint32_t n = SECTOR - bo;
        if (n > endp - p) n = endp - p;
        u_memcpy(sec + bo, data + (p - offset), n);
        if (!disk_write(lba, 1, sec)) return FS_ERR_IO;
        p += n;
    }
    uint32_t new_size = old_size;
    if (offset + len > new_size) new_size = offset + len;
    if (!patch_dir_size(&loc, new_size)) return FS_ERR_IO;
    return FS_OK;
}

static int fs_unlink(const char *path)
{
    uint32_t parent;
    char base[64];
    if (!split_parent(path, &parent, base, sizeof(base))) return FS_ERR_NOENT;
    dir_loc_t loc;
    dir_lookup(parent, base, &loc);
    if (!loc.found) return FS_ERR_NOENT;
    if (loc.is_dir) {
        if (!dir_is_empty(loc.first_clus)) return FS_ERR_IO;  /* 非空目录拒删 */
    }
    if (!fat_free_chain(loc.first_clus)) return FS_ERR_IO;
    uint8_t e5 = 0xE5;
    if (!dir_patch(loc.entry.lba, loc.entry.off, &e5, 1)) return FS_ERR_IO;
    for (int i = 0; i < loc.nlfn; i++) {
        dir_patch(loc.lfn[i].lba, loc.lfn[i].off, &e5, 1);
    }
    return FS_OK;
}

static int fs_rename(const char *oldp, const char *newp)
{
    uint32_t op, np;
    char ob[64], nb[64];
    if (!split_parent(oldp, &op, ob, sizeof(ob))) return FS_ERR_NOENT;
    if (!split_parent(newp, &np, nb, sizeof(nb))) return FS_ERR_NOENT;
    dir_loc_t loc;
    dir_lookup(op, ob, &loc);
    if (!loc.found) return FS_ERR_NOENT;
    uint8_t s83[11];
    if (!make_unique_83(np, nb, s83)) return FS_ERR_IO;
    uint8_t sum;
    lfn_checksum(s83, &sum);
    uint32_t nchars = (uint32_t)u_strlen(nb);
    uint32_t nlfn = (nchars + 12) / 13;
    uint32_t count = nlfn + 1;
    uint32_t xidx;
    if (!dir_find_free_run(np, count, &xidx)) return FS_ERR_IO;
    for (uint32_t i = 0; i < nlfn; i++) {
        uint32_t seq = nlfn - i;
        uint8_t le[32];
        build_lfn_entry(seq, nlfn, sum, nb, le);
        uint32_t c, bo;
        if (!dir_entry_pos(np, xidx + i, &c, &bo)) return FS_ERR_IO;
        if (!write_entry_at(clus_to_lba(c) + bo / SECTOR, bo % SECTOR, le)) return FS_ERR_IO;
    }
    uint8_t e83[32];
    build_83_entry(e83, s83, loc.is_dir ? (uint8_t)0x10 : (uint8_t)0x20,
                   loc.first_clus, loc.size);
    {
        uint32_t c, bo;
        if (!dir_entry_pos(np, xidx + nlfn, &c, &bo)) return FS_ERR_IO;
        if (!write_entry_at(clus_to_lba(c) + bo / SECTOR, bo % SECTOR, e83)) return FS_ERR_IO;
    }
    uint8_t e5 = 0xE5;
    if (!dir_patch(loc.entry.lba, loc.entry.off, &e5, 1)) return FS_ERR_IO;
    for (int i = 0; i < loc.nlfn; i++) {
        dir_patch(loc.lfn[i].lba, loc.lfn[i].off, &e5, 1);
    }
    return FS_OK;
}

static int fs_truncate(const char *path, uint32_t new_size)
{
    uint32_t parent;
    char base[64];
    if (!split_parent(path, &parent, base, sizeof(base))) return FS_ERR_NOENT;
    dir_loc_t loc;
    dir_lookup(parent, base, &loc);
    if (!loc.found) return FS_ERR_NOENT;
    uint32_t clus_bytes = g_sec_per_clus * SECTOR;
    uint32_t cur = (loc.first_clus == 0) ? 0 : chain_len(loc.first_clus);
    uint32_t need = (new_size + clus_bytes - 1) / clus_bytes;
    if (new_size == 0) need = 0;
    if (need > cur) {
        uint32_t start = loc.first_clus;
        uint32_t last = start;
        if (cur == 0) {
            uint32_t nc = fat_alloc_cluster();
            if (nc == 0) return FS_ERR_IO;
            if (!write_zero_cluster(nc)) return FS_ERR_IO;
            start = nc; last = nc; cur = 1;
            if (!patch_dir_firstclus(&loc, nc)) return FS_ERR_IO;
        }
        while (cur < need) {
            uint32_t nc = fat_alloc_cluster();
            if (nc == 0) return FS_ERR_IO;
            if (!write_zero_cluster(nc)) return FS_ERR_IO;
            if (!fat_set(last, nc)) return FS_ERR_IO;
            if (!fat_set(nc, 0x0FFFFFFF)) return FS_ERR_IO;
            last = nc; cur++;
        }
    } else if (need < cur) {
        if (need == 0) {
            if (!fat_free_chain(loc.first_clus)) return FS_ERR_IO;
            if (!patch_dir_firstclus(&loc, 0)) return FS_ERR_IO;
        } else {
            uint32_t c = cluster_at_index(loc.first_clus, need - 1);
            uint32_t nx = fat_next(c);
            if (!fat_set(c, 0x0FFFFFFF)) return FS_ERR_IO;   /* 截断 */
            if (nx >= 2 && nx - 2 < g_total_clusters) {
                fat_free_chain(nx);
            }
        }
    }
    if (!patch_dir_size(&loc, new_size)) return FS_ERR_IO;
    return FS_OK;
}

static int fs_cmp(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    }
    return 0;
}

static int fs_read_all(const char *path, uint8_t *buf, uint32_t cap)
{
    path_cb_t f;
    resolve_path(path, &f);
    if (!f.found || f.is_dir) return -1;
    return (int)read_file(f.clus, f.size, (char *)buf, cap);
}

/* 挂载自检：写 -> 读回 -> 比较，覆盖创建/追加/子目录/重命名/删除/截断。 */
static void fs_selftest(void)
{
    u_print("[fs] write-path self-test begin\n");
    bool ok = true;
    const char *msg = "SukiOS FAT32 write path OK!\n";
    uint32_t mlen = (uint32_t)u_strlen(msg);
    if (fs_write("WRITETST.TXT", (const uint8_t *)msg, mlen, 0) != FS_OK) {
        u_print("[fs]   create+write FAIL\n"); ok = false;
    } else {
        uint8_t rb[64];
        int n = fs_read_all("WRITETST.TXT", rb, sizeof(rb));
        if (n != (int)mlen || fs_cmp(rb, (const uint8_t *)msg, mlen) != 0) {
            u_print("[fs]   write+readback MISMATCH\n"); ok = false;
        } else u_print("[fs]   write+readback PASS\n");
    }
    if (fs_write("WRITETST.TXT", (const uint8_t *)"APPEND", 6, mlen) == FS_OK) {
        uint8_t rb[64];
        int n = fs_read_all("WRITETST.TXT", rb, sizeof(rb));
        if (n == (int)(mlen + 6) && fs_cmp(rb, (const uint8_t *)msg, mlen) == 0
            && fs_cmp(rb + mlen, (const uint8_t *)"APPEND", 6) == 0) {
            u_print("[fs]   append PASS\n");
        } else { u_print("[fs]   append FAIL\n"); ok = false; }
    } else { u_print("[fs]   append FAIL\n"); ok = false; }
    if (fs_create("TESTDIR", true) != FS_OK) {
        u_print("[fs]   mkdir FAIL\n"); ok = false;
    } else if (fs_write("TESTDIR/INNER.TXT", (const uint8_t *)"inner ok\n", 8, 0) != FS_OK) {
        u_print("[fs]   subdir write FAIL\n"); ok = false;
    } else {
        uint8_t rb[32];
        int n = fs_read_all("TESTDIR/INNER.TXT", rb, sizeof(rb));
        if (n == 8 && fs_cmp(rb, (const uint8_t *)"inner ok\n", 8) == 0) {
            u_print("[fs]   mkdir+subdir PASS\n");
        } else { u_print("[fs]   subdir readback FAIL\n"); ok = false; }
    }
    if (fs_rename("WRITETST.TXT", "WT2.TXT") == FS_OK) {
        uint8_t rb[64];
        int n = fs_read_all("WT2.TXT", rb, sizeof(rb));
        if (n == (int)(mlen + 6) && fs_cmp(rb, (const uint8_t *)msg, mlen) == 0
            && fs_cmp(rb + mlen, (const uint8_t *)"APPEND", 6) == 0) {
            u_print("[fs]   rename PASS\n");
        } else { u_print("[fs]   rename FAIL\n"); ok = false; }
    } else { u_print("[fs]   rename FAIL\n"); ok = false; }
    if (fs_unlink("WT2.TXT") != FS_OK) {
        u_print("[fs]   unlink FAIL\n"); ok = false;
    } else {
        path_cb_t f; resolve_path("WT2.TXT", &f);
        if (f.found) { u_print("[fs]   unlink FAIL (still found)\n"); ok = false; }
        else u_print("[fs]   unlink PASS\n");
    }
    if (fs_unlink("TESTDIR/INNER.TXT") != FS_OK) {
        u_print("[fs]   unlink inner FAIL\n"); ok = false;
    }
    if (fs_unlink("TESTDIR") != FS_OK) {
        u_print("[fs]   rmdir FAIL\n"); ok = false;
    } else {
        path_cb_t f; resolve_path("TESTDIR", &f);
        if (f.found) { u_print("[fs]   rmdir FAIL\n"); ok = false; }
        else u_print("[fs]   rmdir PASS\n");
    }
    if (fs_write("TRUNC.TXT", (const uint8_t *)"0123456789", 10, 0) == FS_OK) {
        if (fs_truncate("TRUNC.TXT", 4) == FS_OK) {
            uint8_t rb[16];
            int n = fs_read_all("TRUNC.TXT", rb, sizeof(rb));
            if (n == 4 && fs_cmp(rb, (const uint8_t *)"0123", 4) == 0) {
                u_print("[fs]   truncate PASS\n");
            } else { u_print("[fs]   truncate FAIL\n"); ok = false; }
        } else { u_print("[fs]   truncate FAIL\n"); ok = false; }
        fs_unlink("TRUNC.TXT");
    }
    u_print(ok ? "[fs] write-path self-test: ALL PASS\n"
                : "[fs] write-path self-test: FAIL\n");
}

/* ---- 服务循环 ---- */
static uint8_t g_req[sizeof(mach_msg_header_t) + sizeof(fs_write_req_t) + 96 + FS_WRITE_MAX];
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
        } else if (rh->msgh_id == FS_MSG_CREATE) {
            char *name = (char *)g_req + sizeof(*rh);
            g_req[sizeof(g_req) - 1] = 0;
            fr->status = fs_create(name, false);
            fr->length = 0;                      /* 写类应答不带数据体 */
        } else if (rh->msgh_id == FS_MSG_MKDIR) {
            char *name = (char *)g_req + sizeof(*rh);
            g_req[sizeof(g_req) - 1] = 0;
            fr->status = fs_create(name, true);
            fr->length = 0;
        } else if (rh->msgh_id == FS_MSG_WRITE) {
            fs_write_req_t *wr = (fs_write_req_t *)(g_req + sizeof(*rh));
            char *name = (char *)(g_req + sizeof(*rh) + sizeof(*wr));
            uint32_t maxn = (uint32_t)(sizeof(g_req) - (sizeof(*rh) + sizeof(*wr)));
            uint32_t namelen = 0;
            while (namelen < maxn && name[namelen]) namelen++;
            if (namelen >= maxn) {
                fr->status = FS_ERR_IO;
            } else {
                const uint8_t *data = (const uint8_t *)(name + namelen + 1);
                uint32_t len = wr->length;
                uint32_t avail = (uint32_t)(sizeof(g_req)
                                - (sizeof(*rh) + sizeof(*wr) + namelen + 1));
                if (len > avail) len = avail;
                if (len > FS_WRITE_MAX) len = FS_WRITE_MAX;
                fr->status = fs_write(name, data, len, wr->offset);
            }
            fr->length = 0;
        } else if (rh->msgh_id == FS_MSG_UNLINK) {
            char *name = (char *)g_req + sizeof(*rh);
            g_req[sizeof(g_req) - 1] = 0;
            fr->status = fs_unlink(name);
            fr->length = 0;
        } else if (rh->msgh_id == FS_MSG_RENAME) {
            fs_rename_req_t *rr = (fs_rename_req_t *)(g_req + sizeof(*rh));
            fr->status = fs_rename(rr->old_name, rr->new_name);
            fr->length = 0;
        } else if (rh->msgh_id == FS_MSG_TRUNCATE) {
            fs_trunc_req_t *tr = (fs_trunc_req_t *)(g_req + sizeof(*rh));
            char *name = (char *)(g_req + sizeof(*rh) + sizeof(*tr));
            g_req[sizeof(g_req) - 1] = 0;
            fr->status = fs_truncate(name, tr->size);
            fr->length = 0;
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
        u_print(u_utoa_s(size, num, sizeof(num)));
        u_print(" bytes");
    }
    u_print("\n");
    return false;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    u_print("[fs] FS_SERVER starting (Ring3 FAT32, read/write)\n");
    /* A2 项：认领本服务的接收端口（否则内核会因 owner 不匹配拒绝接收） */
    sys_port_claim(FS_PORT);
    sys_port_claim(FS_REPLY_PORT);
    if (!fat32_mount()) {
        u_print("[fs] mount FAILED\n");
        return 1;
    }
    u_print("[fs] root directory (self-test):\n");
    walk_root(selftest_cb, 0);
    fs_selftest();
    u_print("[fs] entering service loop on FS_PORT\n");
    serve();
    return 0;
}
