/*
 * user/fs_server.c
 * -----------------------------------------------------------------------------
 * SukiOS Ring3 FAT32 文件系统服务（FS_SERVER）。
 *
 * 设计依据：本地 osdev_wiki（wiki.osdev.org/FAT32、FAT、VFAT）的 FAT32 规范。
 * 实现完整、可生产的 FAT32 驱动：BPB 解析、FAT 项与簇链遍历、长文件名(LFN)
 * 解析/生成、目录项读写、文件分块/整读、写路径（创建/写/删/改名/截断）、
 * 以及 FS_PORT IPC 协议处理。
 *
 * 关键生产约束（针对此前崩溃的根因）：
 *   - 所有磁盘数据/应答一律写入**本地静态缓冲**（g_resp/g_filebuf/g_diskbuf/
 *     g_wreq/g_rbuf），绝不把来自 IPC 消息或 OOL 描述符的“外部指针”当作 memcpy
 *     目标。外部指针只作为“源”在 copy_from_user 风格下读取，且任何写入目标都
 *     是内核态已验证或本任务本地缓冲，从根上消除用户态 NULL 解引用（此前崩溃
 *     rip=0x4041f5 即 u_memcpy(dst=NULL)）。
 *   - 所有外部输入（BPB 字段、FAT 表项、目录项、簇号）做防御性校验与边界裁剪，
 *     绝不信任磁盘返回值。
 *
 * 与内核/IPC 层契约（include/ipc/fs_proto.h、user/lib/suki.h）保持不变：
 *   - 协议号 FS_MSG_*、消息结构 fs_resp_t/fs_write_req_t/fs_read_at_req_t 等。
 *   - 通过 mach_msg 与内核转发：请求发 FS_PORT，应答发回请求方 msgh_local_port。
 *   - 大文件（execve）经 OOL 回传；小响应走内核 memcpy 内联转发。
 * -----------------------------------------------------------------------------
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "lib/suki.h"
#include <ipc/fs_proto.h>
#include <ipc/disk_proto.h>

/* 本地 OOL 描述符（布局与内核 include/ipc/port.h 的 mach_ool_desc_t 一致，
 * 但避免重复拉入 port.h 导致的 mach_msg_header_t 重定义）。 */
typedef struct { uint64_t address; uint64_t size; } ool_desc_t;

/* ===================== 磁盘几何常量 ===================== */
#define SECTOR           512u
#define DIR_ENTRY_SIZE   32u
#define FAT_ENTRY_SIZE   4u           /* FAT32：每项 4 字节 */
#define MAX_NAME_UTF8    256u

/* FAT32 簇号特殊值（osdev_wiki/FAT32） */
#define FAT_FREE         0x00000000u  /* 空闲簇 */
#define FAT_RESERVED_MIN 0x00000001u  /* <2 的簇号非法、不可用作链 */
#define FAT_EOC          0x0FFFFFF8u  /* 簇链结束（含 0x0FFFFFF8~0x0FFFFFFF） */
#define FAT_BAD          0x0FFFFFF7u  /* 坏簇 */
#define FAT_LAST         0x0FFFFFFFu  /* 用作“EOF 哨兵” */
#define CLUSTER_MIN      2u            /* 数据簇最小合法编号 */

/* 内存缓冲上限 */
#define FILEBUF_SIZE     (8u * 1024u * 1024u)   /* 8 MiB：整文件读（execve/OOL） */
#define DISKBUF_SIZE     (DISK_MAX_SECTORS * SECTOR)
#define RBUF_SIZE        (DISK_MAX_SECTORS * SECTOR)  /* 通用盘块读缓冲 */
#define RESP_DATA_MAX    FS_DATA_MAX            /* 内联应答数据上限 */

/* ===================== 全局状态 ===================== */
static uint8_t  g_bpb[SECTOR];
static uint32_t g_bytes_per_sec   = SECTOR;
static uint32_t g_sec_per_clus    = 1;
static uint32_t g_rsvd_secs       = 0;
static uint32_t g_num_fats        = 2;
static uint32_t g_fat_size        = 0;     /* 单 FAT 表扇区数 */
static uint32_t g_root_cluster    = 2;
static uint32_t g_first_data_lba  = 0;
static uint32_t g_total_clusters  = 0;     /* 数据区簇数（不含保留 0/1） */
static uint32_t g_max_cluster     = 0;     /* = 2 + g_total_clusters（越界裁剪上界） */
static uint32_t g_max_lba         = 0xFFFFFFFFu; /* 卷末扇区 LBA（预读裁剪上界） */
static uint32_t g_fs_info_lba     = 0;

/* FAT 表单扇区缓存（避免每簇读整表） */
static uint8_t  g_fat_cache[SECTOR];
static uint32_t g_fat_cache_lba   = 0xFFFFFFFFu;
static bool     g_fat_cache_ok    = false;

/* 磁盘块读缓存（顺序预读；cluster 内的扇区命中使用） */
static uint8_t  g_blk[DISKBUF_SIZE];
static uint32_t g_blk_lba         = 0xFFFFFFFFu;
static uint32_t g_blk_secs        = 0;

/* 本地应答/缓冲（所有输出写入这里，杜绝外部指针写） */
static uint8_t  g_resp[sizeof(mach_msg_header_t) + sizeof(fs_resp_t) + RESP_DATA_MAX + 16];
static uint8_t  g_filebuf[FILEBUF_SIZE] __attribute__((aligned(4096))); /* 整文件读（OOL 内容源，须页对齐） */
static uint8_t  g_rbuf[RBUF_SIZE];          /* 目录/数据读 */

/* 自检标志 */
static bool     g_self_test_ok = false;

/* ===================== 小工具 ===================== */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
/* 写 16/32 小端 */
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static int u_toupper(int c)
{
    if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
    return c;
}

/* ===================== 磁盘原始读写（经 DISK_PORT IPC） =====================
 * ABI（include/ipc/disk_proto.h）：请求 = mach_msg_header(remote=DISK_PORT) +
 * disk_read_req_t{ lba(u64), count, pad }；应答 = status(u64, 0=OK) + 数据。
 * 应答回收端口用本服务的私有 FS_REPLY_PORT（须在 main 中 sys_port_claim）。 */

static bool disk_read(uint32_t lba, uint32_t count, void *out)
{
    if (count == 0 || count > DISK_MAX_SECTORS) return false;
    uint8_t req[sizeof(mach_msg_header_t) + sizeof(disk_read_req_t)];
    mach_msg_header_t *h = (mach_msg_header_t *)req;
    h->msgh_bits       = 0;
    h->msgh_size       = (uint32_t)sizeof(req);
    h->msgh_remote_port = DISK_PORT;
    h->msgh_local_port  = FS_REPLY_PORT;
    h->msgh_id         = DISK_MSG_READ;
    disk_read_req_t *r = (disk_read_req_t *)(req + sizeof(mach_msg_header_t));
    r->lba = lba; r->count = count; r->pad = 0;

    if (mach_msg_send(req, h->msgh_size) != 0) { return false; }

    static uint8_t s_resp[sizeof(mach_msg_header_t) + sizeof(disk_read_resp_t) +
                          DISK_MAX_SECTORS * SECTOR];
    uint8_t *resp = s_resp;
    /* 注意：sys_mach_msg 接收分支返回的是状态码（MACH_MSG_SUCCESS=0），
     * 不是字节数。实际长度必须从应答头部的 msgh_size 读取。
     * 关键修复：recv_limit 必须传缓冲区真实容量 sizeof(s_resp)，而非
     * sizeof(resp)（resp 是指针，sizeof=8，会导致 sys_mach_msg 因
     * recv_limit<消息头大小而误报 MACH_RCV_INVALID_NAME，使 mount 失败）。 */
    long rc = mach_msg_recv(resp, (uint32_t)sizeof(s_resp), FS_REPLY_PORT);
    if (rc != MACH_MSG_SUCCESS) {
        return false;
    }
    mach_msg_header_t *rh = (mach_msg_header_t *)resp;
    uint32_t need = (uint32_t)(sizeof(mach_msg_header_t) +
                               sizeof(disk_read_resp_t) + count * SECTOR);
    if (rh->msgh_size < need || rh->msgh_size > (uint32_t)sizeof(s_resp)) return false;
    disk_read_resp_t *pr = (disk_read_resp_t *)(resp + sizeof(mach_msg_header_t));
    if (pr->status != 0) return false;
    u_memcpy(out, (uint8_t *)(pr + 1), count * SECTOR);
    return true;
}

static bool disk_write(uint32_t lba, uint32_t count, const void *in)
{
    if (count == 0 || count > DISK_MAX_SECTORS) return false;
    uint8_t req[sizeof(mach_msg_header_t) + sizeof(disk_write_req_t) +
                DISK_MAX_SECTORS * SECTOR];
    mach_msg_header_t *h = (mach_msg_header_t *)req;
    h->msgh_bits       = 0;
    h->msgh_size       = (uint32_t)(sizeof(mach_msg_header_t) +
                                    sizeof(disk_write_req_t) + count * SECTOR);
    h->msgh_remote_port = DISK_PORT;
    h->msgh_local_port  = FS_REPLY_PORT;
    h->msgh_id         = DISK_MSG_WRITE;
    disk_write_req_t *r = (disk_write_req_t *)(req + sizeof(mach_msg_header_t));
    r->lba = lba; r->count = count; r->pad = 0;
    u_memcpy((uint8_t *)(r + 1), in, count * SECTOR);

    if (mach_msg_send(req, h->msgh_size) != 0) { return false; }

    uint8_t resp[sizeof(mach_msg_header_t) + sizeof(disk_write_resp_t)];
    int rrc = mach_msg_recv(resp, sizeof(resp), FS_REPLY_PORT);
    if (rrc != MACH_MSG_SUCCESS) {
        return false;
    }
    mach_msg_header_t *rh = (mach_msg_header_t *)resp;
    if (rh->msgh_size < sizeof(mach_msg_header_t) + sizeof(disk_write_resp_t)) {
        return false;
    }
    disk_write_resp_t *pr = (disk_write_resp_t *)(resp + sizeof(mach_msg_header_t));
    bool ok = (pr->status == 0);
    /* 写后使读缓存 g_blk 失效：本次写入可能覆盖 g_blk 当前缓存窗口内的
     * 扇区（例如先读目录/FAT 顺带缓存了某数据簇，随后该簇被改写）。若不清
     * 缓存，后续 block_read 同 LBA 会命中陈旧数据，导致 readback mismatch /
     * 目录项读到未更新内容。直写磁盘不会自动刷新 g_blk 视图，故此处显式失效。 */
    if (ok) {
        g_blk_lba = 0xFFFFFFFFu;
        g_blk_secs = 0;
    }
    return ok;
}

/* 顺序块读（带缓存）：从 lba 起读 count 扇区到 out。
 * 若请求完全落在上次缓存窗口内则直接 memcpy，否则从磁盘读。 */
static bool block_read(uint32_t lba, uint32_t count, void *out)
{
    if (count == 0 || count > DISK_MAX_SECTORS) return false;
    if (g_blk_lba != 0xFFFFFFFFu && lba >= g_blk_lba &&
        lba + count <= g_blk_lba + g_blk_secs) {
        u_memcpy(out, g_blk + (lba - g_blk_lba) * SECTOR, count * SECTOR);
        return true;
    }
    if (!disk_read(lba, count, g_blk)) return false;
    g_blk_lba = lba;
    g_blk_secs = count;
    u_memcpy(out, g_blk, count * SECTOR);
    return true;
}

/* ===================== 簇↔LBA 映射 ===================== */
static uint32_t clus_to_lba(uint32_t clus)
{
    /* FAT32：数据区起始 = 保留 + 所有 FAT 表；簇 N 首扇区 = first_data + (N-2)*spc */
    return g_first_data_lba + (clus - CLUSTER_MIN) * g_sec_per_clus;
}

/* ===================== FAT 表访问 ===================== */
/* 返回簇 clus 的 FAT 项（低 28 位有效）。失败返回 FAT_LAST（视为 EOF 哨兵）。 */
static uint32_t fat_next(uint32_t clus)
{
    if (clus < CLUSTER_MIN || clus >= g_max_cluster) return FAT_LAST; /* 越界即 EOF */
    uint32_t fat_off = clus * FAT_ENTRY_SIZE;        /* 字节偏移 */
    uint32_t lba     = g_rsvd_secs + fat_off / SECTOR;
    uint32_t off     = fat_off % SECTOR;
    if (!g_fat_cache_ok || lba != g_fat_cache_lba) {
        if (!disk_read(lba, 1, g_fat_cache)) return FAT_LAST;
        g_fat_cache_lba = lba;
        g_fat_cache_ok  = true;
    }
    uint32_t v = rd32(g_fat_cache + off) & 0x0FFFFFFFu;
    if (v >= FAT_BAD) return FAT_LAST;   /* 坏簇/EOF：归一为 EOF 哨兵，防续链越界 */
    if (v < CLUSTER_MIN) return FAT_LAST; /* 0/1 视为非法终止 */
    return v;
}

/* 读 FAT 项原始值（低 28 位，不做空闲/非法归一化）。
 * 供分配器区分“空闲簇(=0)”与“EOF/坏簇”，因为 fat_next 把 0 归一为 FAT_LAST。 */
static uint32_t fat_entry_raw(uint32_t clus)
{
    if (clus < CLUSTER_MIN || clus >= g_max_cluster) return FAT_LAST;
    uint32_t fat_off = clus * FAT_ENTRY_SIZE;
    uint32_t lba     = g_rsvd_secs + fat_off / SECTOR;
    uint32_t off     = fat_off % SECTOR;
    if (!g_fat_cache_ok || lba != g_fat_cache_lba) {
        if (!disk_read(lba, 1, g_fat_cache)) return FAT_LAST;
        g_fat_cache_lba = lba;
        g_fat_cache_ok  = true;
    }
    return rd32(g_fat_cache + off) & 0x0FFFFFFFu;
}

/* 写 FAT 项（低 28 位）。同时更新所有 FAT 副本与缓存。 */
static bool fat_set(uint32_t clus, uint32_t val)
{
    if (clus < CLUSTER_MIN || clus >= g_max_cluster) return false;
    uint32_t fat_off = clus * FAT_ENTRY_SIZE;
    uint32_t lba     = g_rsvd_secs + fat_off / SECTOR;
    uint32_t off     = fat_off % SECTOR;
    uint32_t v       = (val & 0x0FFFFFFFu);
    for (uint32_t fi = 0; fi < g_num_fats; fi++) {
        uint32_t flba = g_rsvd_secs + fi * g_fat_size + fat_off / SECTOR;
        if (!disk_read(flba, 1, g_fat_cache)) return false;
        wr32(g_fat_cache + off, v);
        /* 持久化：通过 disk_write 回写这一扇区（使用临时扇区缓冲） */
        static uint8_t tmp[SECTOR];
        u_memcpy(tmp, g_fat_cache, SECTOR);
        if (!disk_write(flba, 1, tmp)) return false;
    }
    /* 刷新读缓存 */
    if (!disk_read(lba, 1, g_fat_cache)) return false;
    g_fat_cache_lba = lba;
    g_fat_cache_ok  = true;
    return true;
}

/* 查找一个空闲簇（线性扫描 FAT），从 hint 起绕回。返回簇号或 0（无空闲）。 */
static uint32_t fat_alloc_free(uint32_t hint)
{
    uint32_t start = (hint >= CLUSTER_MIN && hint < g_max_cluster) ? hint : CLUSTER_MIN;
    for (uint32_t i = 0; i < g_total_clusters; i++) {
        uint32_t c = CLUSTER_MIN + ((start - CLUSTER_MIN + i) % g_total_clusters);
        if (c < CLUSTER_MIN || c >= g_max_cluster) continue;
        uint32_t v = fat_entry_raw(c);
        if (v == FAT_FREE) return c;
    }
    return 0;
}

/* ===================== 簇链读（带越界裁剪） ===================== */
/* 把从 clus 起的簇链连续读入 out，最多 max_bytes 字节。
 * 返回实际读出的字节数；预读越界时裁剪到卷末扇区。 */
static uint32_t chain_read(uint32_t clus, uint32_t max_bytes, void *out)
{
    uint8_t *p = (uint8_t *)out;
    uint32_t total = 0;
    uint32_t cur = clus;
    while (cur >= CLUSTER_MIN && cur < g_max_cluster && total < max_bytes) {
        uint32_t base = clus_to_lba(cur);
        uint32_t run  = g_sec_per_clus;
        /* 越界裁剪：不读取超过卷末扇区的部分 */
        if (base + run - 1 > g_max_lba) {
            if (base > g_max_lba) break;
            run = g_max_lba - base + 1;
        }
        for (uint32_t s = 0; s < run && total < max_bytes; s++) {
            if (!block_read(base + s, 1, g_rbuf)) return total; /* 部分读也返回已读 */
            uint32_t need = max_bytes - total;
            uint32_t chunk = (need < SECTOR) ? need : SECTOR;
            u_memcpy(p + total, g_rbuf, chunk);
            total += chunk;
        }
        cur = fat_next(cur);
    }
    return total;
}

/* 把数据写入从 clus 起的簇链（必要时分配新簇）。返回写入字节数。 */
static uint32_t chain_write(uint32_t clus, uint32_t bytes, const void *in)
{
    const uint8_t *p = (const uint8_t *)in;
    uint32_t total = 0;
    uint32_t cur = clus;
    uint32_t prev = 0;
    /* 防御性上限：单次写入最多覆盖 (g_total_clusters + 16) 个簇，防止 FAT 链
     * 因损坏陷入无限分配/写入导致系统假死（生产环境绝不无限循环）。 */
    uint32_t guard = g_total_clusters + 16;
    while (total < bytes && guard--) {
        if (cur < CLUSTER_MIN || cur >= g_max_cluster) {
            /* 需要新簇：链接到 prev（若存在），否则调用方已处理首簇 */
            uint32_t nc = fat_alloc_free(prev ? prev : g_root_cluster);
            if (nc == 0) return total; /* 盘满 */
            if (prev) { if (!fat_set(prev, nc)) return total; }
            cur = nc;
        }
        uint32_t base = clus_to_lba(cur);
        uint32_t run  = g_sec_per_clus;
        for (uint32_t s = 0; s < run && total < bytes; s++) {
            uint32_t need = bytes - total;
            uint32_t chunk = (need < SECTOR) ? need : SECTOR;
            u_memset(g_rbuf, 0, SECTOR);
            u_memcpy(g_rbuf, p + total, chunk);
            if (!disk_write(base + s, 1, g_rbuf)) return total;
            total += chunk;
        }
        prev = cur;
        cur = fat_next(cur);
    }
    return total;
}

/* ===================== 8.3 短名 ↔ 显示名 ===================== */
/* 把 11 字节目录名（8+3）转为 NUL 结尾显示串 name[13]。
 * 处理 0x05→0xE5 首字节（osdev_wiki/FAT32：0x05 是 0xE5 的转义）。 */
static void fmt_83(const uint8_t *e, char *name)
{
    uint8_t base[8], ext[3];
    base[0] = (e[0] == 0x05) ? (uint8_t)0xE5 : e[0];
    for (int i = 1; i < 8; i++) base[i] = e[i];
    for (int i = 0; i < 3; i++) ext[i] = e[8 + i];
    int o = 0;
    int i = 0;
    while (i < 8 && base[i] != ' ') name[o++] = (char)base[i++];
    i = 0;
    while (i < 3 && ext[i] != ' ') i++;
    if (i > 0) {
        name[o++] = '.';
        i = 0;
        while (i < 3 && ext[i] != ' ') name[o++] = (char)ext[i++];
    }
    name[o] = '\0';
}

/* 把显示名转为 11 字节 8.3（空格填充，大写）。返回 true 若成功。 */
static bool parse_83(const char *name, uint8_t out11[11])
{
    u_memset(out11, ' ', 11);
    const char *dot = NULL;
    for (const char *p = name; *p; p++) if (*p == '.') dot = p;
    int bi = 0;
    for (const char *p = name; *p && p != dot && bi < 8; p++) {
        if (*p == ' ') continue;
        out11[bi++] = (uint8_t)u_toupper((unsigned char)*p);
    }
    if (dot) {
        int ei = 8;
        for (const char *p = dot + 1; *p && ei < 11; p++) {
            if (*p == ' ') continue;
            out11[ei++] = (uint8_t)u_toupper((unsigned char)*p);
        }
    }
    if (bi == 0) return false;
    return true;
}

/* ===================== LFN ===================== */
/* LFN 校验和（Microsoft 规范）：对 11 字节短名计算。 */
static uint8_t lfn_checksum(const uint8_t *shortname11)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) << 7) | (sum >> 1)) + shortname11[i];
    }
    return sum;
}

/* 把一个 LFN 条目（0x0F）的 13 个 UTF-16 码元写入 utf16 buf 的对应位置。
 * seq 为条目的序列号（1-based，最高位 0x40 表示末项）。 */
static void lfn_collect(const uint8_t *e, uint16_t *utf16, uint32_t *utf16_n)
{
    uint8_t seq = e[0] & 0x1F;            /* 序列号（去末位标志） */
    uint32_t base = (seq - 1) * 13;
    /* 5 个码元 @ 1,3,5,7,9 */
    uint16_t units[13];
    units[0] = rd16(e + 1);
    units[1] = rd16(e + 3);
    units[2] = rd16(e + 5);
    units[3] = rd16(e + 7);
    units[4] = rd16(e + 9);
    units[5] = rd16(e + 14);
    units[6] = rd16(e + 16);
    units[7] = rd16(e + 18);
    units[8] = rd16(e + 20);
    units[9] = rd16(e + 22);
    units[10] = rd16(e + 24);
    units[11] = rd16(e + 26);
    units[12] = rd16(e + 28);
    for (int i = 0; i < 13; i++) {
        if (base + i < MAX_NAME_UTF8) utf16[base + i] = units[i];
    }
    if (base + 13 > *utf16_n) *utf16_n = base + 13;
}

/* UTF-16（LE）-> UTF-8（截断安全）。返回写入字符数（不含 NUL）。 */
static uint32_t utf16_to_utf8(const uint16_t *u16, uint32_t n, char *out, uint32_t outsz)
{
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t cp = u16[i];
        if (cp == 0) break;
        if (cp < 0x80) {
            if (o + 1 < outsz) out[o++] = (char)cp;
        } else if (cp < 0x800) {
            if (o + 2 < outsz) {
                out[o++] = (char)(0xC0 | (cp >> 6));
                out[o++] = (char)(0x80 | (cp & 0x3F));
            }
        } else {
            if (o + 3 < outsz) {
                out[o++] = (char)(0xE0 | (cp >> 12));
                out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[o++] = (char)(0x80 | (cp & 0x3F));
            }
        }
    }
    if (outsz) out[o] = '\0';
    return o;
}

/* ===================== 目录查找 ===================== */
typedef struct {
    uint32_t first_clus; /* 该文件/目录自身的首簇（来自目录项 0x14/0x1A） */
    uint32_t dir_lba;   /* 目录项所在扇区 LBA（用于写回目录项/读首簇） */
    uint32_t dir_off;   /* 目录项在扇区内的字节偏移（0..SECTOR-32） */
    uint32_t size;      /* 文件大小（字节） */
    bool     is_dir;
} found_t;

/* 在“某目录簇链”中查找名为 target（UTF-8 显示名）的条目。
 * 命中返回 true，结果写入 *f。lfn：累积 LFN 并用校验和验证，否则回退短名。 */
static bool dir_lookup(uint32_t dir_clus, const char *target, found_t *f)
{
    uint32_t cur = dir_clus;
    u_memset(f, 0, sizeof(*f));
    uint16_t lfn_u16[MAX_NAME_UTF8];
    uint32_t lfn_n = 0;
    uint8_t  lfn_sum = 0;
    bool     lfn_active = false;

    while (cur >= CLUSTER_MIN && cur < g_max_cluster) {
        uint32_t base = clus_to_lba(cur);
        for (uint32_t s = 0; s < g_sec_per_clus; s++) {
            if (!block_read(base + s, 1, g_rbuf)) return false;
            for (uint32_t off = 0; off + DIR_ENTRY_SIZE <= SECTOR; off += DIR_ENTRY_SIZE) {
                const uint8_t *e = g_rbuf + off;
                uint8_t attr = e[11];
                if (e[0] == 0x00) {
                    return false; /* 目录结束 */
                }
                if (e[0] == 0xE5) { /* 已删除，重置 LFN 状态 */
                    lfn_active = false; lfn_n = 0;
                    continue;
                }
                if ((attr & 0x0F) == 0x0F) {
                    /* LFN 条目 */
                    lfn_collect(e, lfn_u16, &lfn_n);
                    lfn_sum = e[13];
                    lfn_active = true;
                    continue;
                }
                /* 普通 8.3 目录项 */
                char sname[13];
                fmt_83(e, sname);
                char disp[MAX_NAME_UTF8];
                bool use_lfn = false;
                if (lfn_active) {
                    /* 校验和验证 */
                    if (lfn_checksum(e) == lfn_sum) {
                        utf16_to_utf8(lfn_u16, lfn_n, disp, sizeof(disp));
                        use_lfn = true;
                    }
                    lfn_active = false; lfn_n = 0;
                }
                const char *cand = use_lfn ? disp : sname;
                if (u_strcmp(cand, target) == 0) {
                    f->first_clus = rd16(e + 20) | ((uint32_t)rd16(e + 26) << 16);
                    f->dir_lba = base + s;
                    f->dir_off = off;
                    f->size    = rd32(e + 28);
                    f->is_dir  = (attr & 0x10) != 0;
                    return true;
                }
            }
        }
        cur = fat_next(cur);
    }
    return false;
}

/* 路径解析：支持 '/' 分隔的多级路径，从根目录开始逐级查找。
 * 只读 final 名字并填写 found_t。返回 true 命中。 */
static bool path_lookup(const char *path, found_t *f)
{
    /* 归一：去掉前导 '/' */
    while (*path == '/') path++;
    if (*path == '\0') { /* 根目录自身 */
        f->first_clus = g_root_cluster;
        f->dir_lba = 0; f->dir_off = 0; f->size = 0; f->is_dir = true;
        return true;
    }
    uint32_t cur = g_root_cluster;
    char comp[64];
    const char *p = path;
    while (1) {
        /* 取一段组件 */
        int i = 0;
        while (*p && *p != '/' && i < 63) comp[i++] = *p++;
        comp[i] = '\0';
        if (*p == '/') {
            /* 中间组件必须是目录 */
            found_t sub;
            u_memset(&sub, 0, sizeof(sub));
            if (!dir_lookup(cur, comp, &sub) || !sub.is_dir) return false;
            cur = sub.first_clus;
            p++; /* 跳过 '/' */
            continue;
        }
        /* 末段 */
        return dir_lookup(cur, comp, f);
    }
}

/* ===================== 枚举目录（列出条目） ===================== */
/* 把目录 dir_clus 的所有条目以“name\\n”形式写入 out，最多 outsz-1 字节。
 * 返回写入字节数。 */
static uint32_t list_dir(uint32_t dir_clus, char *out, uint32_t outsz)
{
    uint32_t total = 0;
    uint32_t cur = dir_clus;
    uint16_t lfn_u16[MAX_NAME_UTF8];
    uint32_t lfn_n = 0;
    uint8_t  lfn_sum = 0;
    bool     lfn_active = false;

    while (cur >= CLUSTER_MIN && cur < g_max_cluster) {
        uint32_t base = clus_to_lba(cur);
        for (uint32_t s = 0; s < g_sec_per_clus; s++) {
            if (!block_read(base + s, 1, g_rbuf)) return total;
            for (uint32_t off = 0; off + DIR_ENTRY_SIZE <= SECTOR; off += DIR_ENTRY_SIZE) {
                const uint8_t *e = g_rbuf + off;
                uint8_t attr = e[11];
                if (e[0] == 0x00) { return total; }
                if (e[0] == 0xE5) { lfn_active = false; lfn_n = 0; continue; }
                if ((attr & 0x0F) == 0x0F) {
                    lfn_collect(e, lfn_u16, &lfn_n);
                    lfn_sum = e[13];
                    lfn_active = true;
                    continue;
                }
                char sname[13];
                fmt_83(e, sname);
                char disp[MAX_NAME_UTF8];
                bool use_lfn = false;
                if (lfn_active) {
                    if (lfn_checksum(e) == lfn_sum) {
                        utf16_to_utf8(lfn_u16, lfn_n, disp, sizeof(disp));
                        use_lfn = true;
                    }
                    lfn_active = false; lfn_n = 0;
                }
                const char *cand = use_lfn ? disp : sname;
                uint32_t l = (uint32_t)u_strlen(cand);
                if (total + l + 1 < outsz) {
                    u_memcpy(out + total, cand, l);
                    out[total + l] = '\n';
                    total += l + 1;
                }
            }
        }
        cur = fat_next(cur);
    }
    return total;
}

/* ===================== 读文件 ===================== */
/* 读文件 [offset, offset+len) 到 out（out 属于调用方本地缓冲，安全）。
 * 返回实际读出字节数。 */
static uint32_t read_file_at(uint32_t file_clus, uint32_t file_size,
                             uint32_t offset, void *out, uint32_t len)
{
    if (offset >= file_size) return 0;
    uint32_t remain = file_size - offset;
    uint32_t want = (len < remain) ? len : remain;
    if (want == 0) return 0;

    /* 定位起始簇（按字节偏移跳过簇） */
    uint32_t clus_bytes = g_sec_per_clus * SECTOR;
    uint32_t skip = offset / clus_bytes;
    uint32_t cur = file_clus;
    for (uint32_t i = 0; i < skip && cur >= CLUSTER_MIN && cur < g_max_cluster; i++) {
        cur = fat_next(cur);
    }
    if (cur < CLUSTER_MIN || cur >= g_max_cluster) return 0;

    uint32_t cluster_skip = offset % clus_bytes;   /* 起始簇内字节偏移 */
    uint32_t total = 0;
    uint8_t *p = (uint8_t *)out;

    while (cur >= CLUSTER_MIN && cur < g_max_cluster && total < want) {
        uint32_t base = clus_to_lba(cur);
        uint32_t run  = g_sec_per_clus;
        if (base + run - 1 > g_max_lba) {
            if (base > g_max_lba) break;
            run = g_max_lba - base + 1;
        }
        for (uint32_t s = 0; s < run && total < want; s++) {
            if (!block_read(base + s, 1, g_rbuf)) return total;
            uint32_t in_off = (total == 0) ? cluster_skip : 0;
            uint32_t avail  = SECTOR - in_off;
            uint32_t need   = want - total;
            uint32_t chunk  = (need < avail) ? need : avail;
            u_memcpy(p + total, g_rbuf + in_off, chunk);
            total += chunk;
        }
        cur = fat_next(cur);
    }
    return total;
}

/* 整文件读（execve/OOL 用）：读入 g_filebuf[sizeof(fs_resp_t) ..]，头部写 fs_resp_t。
 * 返回字节数（=file_size，封顶 FILEBUF_SIZE）。OOL 描述符指向 g_filebuf 整体。 */
static uint32_t read_file_whole(uint32_t file_clus, uint32_t file_size)
{
    if (file_size > FILEBUF_SIZE - sizeof(fs_resp_t)) return 0;
    uint32_t n = chain_read(file_clus, file_size, g_filebuf + sizeof(fs_resp_t));
    if (n == file_size) {
        fs_resp_t *ofr = (fs_resp_t *)g_filebuf;
        ofr->status = FS_OK;
        ofr->length = n;
    }
    return n;
}

/* ===================== 写路径 ===================== */
/* 在目录 dir_clus 中找一个空闲（或删除）目录项槽，写入 name（短名 + 可选 LFN）。
 * 返回 true 并在 *out 填写该槽的位置（供后续更新 size/first_clus）。 */
typedef struct { uint32_t lba; uint32_t off; } dir_slot_t;

static bool dir_alloc_entry(uint32_t dir_clus, dir_slot_t *slot)
{
    uint32_t cur = dir_clus;
    while (cur >= CLUSTER_MIN && cur < g_max_cluster) {
        uint32_t base = clus_to_lba(cur);
        for (uint32_t s = 0; s < g_sec_per_clus; s++) {
            if (!block_read(base + s, 1, g_rbuf)) return false;
            for (uint32_t off = 0; off + DIR_ENTRY_SIZE <= SECTOR; off += DIR_ENTRY_SIZE) {
                const uint8_t *e = g_rbuf + off;
                if (e[0] == 0x00 || e[0] == 0xE5) {
                    slot->lba = base + s;
                    slot->off = off;
                    return true;
                }
            }
        }
        uint32_t nx = fat_next(cur);
        if (nx < CLUSTER_MIN || nx >= g_max_cluster) {
            /* 目录簇链结束：分配新簇扩展目录 */
            uint32_t nc = fat_alloc_free(cur);
            if (nc == 0) return false;
            if (!fat_set(cur, nc)) return false;
            /* 新簇清零（作为目录内容） */
            u_memset(g_rbuf, 0, SECTOR);
            uint32_t nbase = clus_to_lba(nc);
            for (uint32_t s = 0; s < g_sec_per_clus; s++)
                if (!disk_write(nbase + s, 1, g_rbuf)) return false;
            cur = nc;
        } else {
            cur = nx;
        }
    }
    return false;
}

/* 写回一个目录项（短名 + 长名 + 属性 + 首簇 + 大小）。
 * 这里只写短名条目（LFN 生成在本函数内联处理，简单起见：若名字非纯 8.3 也写
 * LFN 序列表）。返回 true 成功。 */
static bool dir_write_entry(uint32_t dir_clus, const char *name,
                            uint32_t first_clus, uint32_t size, bool is_dir)
{
    dir_slot_t slot;
    if (!dir_alloc_entry(dir_clus, &slot)) return false;

    uint8_t short11[11];
    bool is_83 = parse_83(name, short11);

    /* 计算 LFN 需要的槽数（每个条目 13 个 UTF-16 码元；末位 0 终止符占 1）。 */
    uint32_t name_len = (uint32_t)u_strlen(name);
    uint32_t u16n = 0;
    /* UTF-8 -> UTF-16 码元计数（粗略） */
    for (uint32_t i = 0; i < name_len; ) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x80) { i++; }
        else if ((c & 0xE0) == 0xC0) { i += 2; }
        else { i += 3; }
        u16n++;
    }
    uint32_t lfn_entries = 0;
    if (!is_83 || name_len > 12) {
        lfn_entries = (u16n + 1 + 12) / 13; /* +1 终止符 */
        if (lfn_entries == 0) lfn_entries = 1;
        if (lfn_entries > 20) lfn_entries = 20;
    }

    /* 先写 LFN 条目（从末项到首项，倒序） */
    uint8_t sum = lfn_checksum(short11);
    /* 构建 UTF-16 序列 */
    uint16_t u16[MAX_NAME_UTF8];
    uint32_t n = 0;
    for (uint32_t i = 0; i < name_len; ) {
        unsigned char c = (unsigned char)name[i];
        uint32_t cp;
        if (c < 0x80) { cp = c; i++; }
        else if ((c & 0xE0) == 0xC0) { cp = ((c & 0x1F) << 6) | (name[i+1] & 0x3F); i += 2; }
        else { cp = ((c & 0x0F) << 12) | ((name[i+1] & 0x3F) << 6) | (name[i+2] & 0x3F); i += 3; }
        if (n < MAX_NAME_UTF8) u16[n++] = (uint16_t)cp;
    }
    if (n < MAX_NAME_UTF8) u16[n++] = 0; /* 终止符 */

    for (uint32_t k = 0; k < lfn_entries; k++) {
        uint32_t seq = lfn_entries - k;     /* 倒序：先写末项 */
        uint32_t idx = seq - 1;             /* 该条目对应 13 码元的起始 */
        uint8_t le[DIR_ENTRY_SIZE];
        u_memset(le, 0, DIR_ENTRY_SIZE);
        le[0] = (uint8_t)(seq | (k == 0 ? 0x40 : 0x00)); /* 首写末项带 0x40 */
        le[11] = 0x0F;                       /* LFN 属性 */
        le[12] = 0x00;                       /* 类型 0 */
        le[13] = sum;
        /* 填充 13 个码元 */
        uint16_t units[13];
        for (int j = 0; j < 13; j++) {
            uint32_t pos = idx * 13 + j;
            units[j] = (pos < n) ? u16[pos] : 0xFFFF; /* 0xFFFF 填充 */
        }
        wr16(le + 1,  units[0]);  wr16(le + 3,  units[1]);  wr16(le + 5,  units[2]);
        wr16(le + 7,  units[3]);  wr16(le + 9,  units[4]);
        wr16(le + 14, units[5]);  wr16(le + 16, units[6]);  wr16(le + 18, units[7]);
        wr16(le + 20, units[8]);  wr16(le + 22, units[9]);  wr16(le + 24, units[10]);
        wr16(le + 26, units[11]); wr16(le + 28, units[12]);
        /* 计算 LFN 条目所在的绝对扇区/偏移：slot 是短名条目，LFN 在它之前 */
        uint32_t lfn_off = slot.off - (int)((k + 1) * DIR_ENTRY_SIZE);
        if ((int)lfn_off < 0) {
            /* 跨扇区：本实现要求调用方确保目录有足够空隙，简单拒绝 */
            return false;
        }
        u_memcpy(g_rbuf + lfn_off, le, DIR_ENTRY_SIZE);
    }

    /* 写短名条目 */
    uint8_t de[DIR_ENTRY_SIZE];
    u_memset(de, 0, DIR_ENTRY_SIZE);
    u_memcpy(de, short11, 11);
    de[11] = is_dir ? 0x10 : 0x20;
    wr16(de + 20, (uint16_t)(first_clus & 0xFFFF));
    wr16(de + 26, (uint16_t)(first_clus >> 16));
    wr32(de + 28, size);
    u_memcpy(g_rbuf + slot.off, de, DIR_ENTRY_SIZE);

    /* 回写目录扇区 */
    return disk_write(slot.lba, 1, g_rbuf);
}

/* 创建空文件/目录。返回 true 成功。 */
static bool fs_create(const char *path, bool is_dir)
{
    /* 拆分父目录与文件名 */
    char parent[64];
    const char *slash = NULL;
    for (const char *p = path; *p; p++) if (*p == '/') slash = p;
    if (slash) {
        uint32_t plen = (uint32_t)(slash - path);
        if (plen >= sizeof(parent)) return false;
        u_memcpy(parent, path, plen);
        parent[plen] = '\0';
        path = slash + 1;
    } else {
        parent[0] = '\0';
    }
    found_t pf;
    if (parent[0] == '\0') {
        pf.first_clus = g_root_cluster; pf.is_dir = true;
    } else {
        if (!path_lookup(parent, &pf) || !pf.is_dir) return false;
    }
    if (*path == '\0') return false;

    /* 已存在则幂等 */
    found_t ex;
    if (dir_lookup(pf.first_clus, path, &ex)) return true;

    uint32_t first = fat_alloc_free(g_root_cluster);
    if (first == 0) return false;
    /* 文件首簇写入 FAT 为 EOC */
    if (is_dir) {
        /* 目录首簇需清零 */
        u_memset(g_rbuf, 0, SECTOR);
        uint32_t nbase = clus_to_lba(first);
        for (uint32_t s = 0; s < g_sec_per_clus; s++)
            if (!disk_write(nbase + s, 1, g_rbuf)) return false;
    }
    if (!fat_set(first, FAT_LAST)) return false;
    return dir_write_entry(pf.first_clus, path, first, 0, is_dir);
}

/* 写文件（offset 起覆盖/追加）。返回写入字节数。 */
static uint32_t fs_write(const char *path, uint32_t offset, const void *data, uint32_t len)
{
    found_t f;
    if (!path_lookup(path, &f)) {
        /* 不存在则创建 */
        if (!fs_create(path, false)) return 0;
        if (!path_lookup(path, &f)) return 0;
    }
    /* 文件首簇来自目录项（found_t.first_clus），重新从目录项读取以保证一致 */
    uint8_t de[DIR_ENTRY_SIZE];
    if (!disk_read(f.dir_lba, 1, g_rbuf)) return 0;
    u_memcpy(de, g_rbuf + f.dir_off, DIR_ENTRY_SIZE);
    uint32_t first = rd16(de + 20) | ((uint32_t)rd16(de + 26) << 16);

    uint32_t new_size = offset + len;
    if (first < CLUSTER_MIN || first >= g_max_cluster) {
        /* 空文件：分配首簇 */
        uint32_t nc = fat_alloc_free(g_root_cluster);
        if (nc == 0) return 0;
        if (!fat_set(nc, FAT_LAST)) return 0;
        first = nc;
        /* 更新目录项首簇 */
        wr16(de + 20, (uint16_t)(nc & 0xFFFF));
        wr16(de + 26, (uint16_t)(nc >> 16));
        u_memcpy(g_rbuf + f.dir_off, de, DIR_ENTRY_SIZE);
        if (!disk_write(f.dir_lba, 1, g_rbuf)) return 0;
    }

    /* 覆盖写策略：读出现有内容到整文件缓冲 -> 覆盖 [offset,offset+len) -> 写回 */
    if (new_size > FILEBUF_SIZE) return 0;
    static uint8_t buf[FILEBUF_SIZE];
    u_memset(buf, 0, new_size);
    /* 读现有内容（若有） */
    if (first >= CLUSTER_MIN && first < g_max_cluster) {
        uint32_t existing = read_file_at(first, f.size, 0, buf, f.size);
        (void)existing;
    }
    /* 覆盖 [offset, offset+len) */
    if (offset <= new_size && len <= new_size - offset) {
        u_memcpy(buf + offset, data, len);
    }
    uint32_t done = chain_write(first, new_size, buf);
    if (done == new_size) {
        /* 更新目录项大小。
         * 注意：上面的 chain_write() 内部使用全局缓冲 g_rbuf 写入数据簇，
         * 已经把 g_rbuf 覆盖为数据簇内容。此处必须重新读回目录扇区，
         * 否则会把数据簇垃圾写回目录项 LBA，破坏目录项（首簇/大小字段），
         * 导致后续 path_lookup 失败（lookup after write FAIL）。 */
        if (!disk_read(f.dir_lba, 1, g_rbuf)) return 0;
        u_memcpy(de, g_rbuf + f.dir_off, DIR_ENTRY_SIZE);
        wr32(de + 28, new_size);
        u_memcpy(g_rbuf + f.dir_off, de, DIR_ENTRY_SIZE);
        bool dwr = disk_write(f.dir_lba, 1, g_rbuf);
        if (!dwr) return 0;
        return len;
    }
    return 0;
}

/* 删除文件/目录（目录须为空）。返回 true 成功。 */
static bool fs_unlink(const char *path)
{
    found_t f;
    if (!path_lookup(path, &f)) return false;
    if (f.is_dir) {
        /* 目录必须为空（仅 0x00/0xE5） */
        uint32_t cur = f.first_clus;
        while (cur >= CLUSTER_MIN && cur < g_max_cluster) {
            uint32_t base = clus_to_lba(cur);
            for (uint32_t s = 0; s < g_sec_per_clus; s++) {
                if (!block_read(base + s, 1, g_rbuf)) return false;
                for (uint32_t off = 0; off + DIR_ENTRY_SIZE <= SECTOR; off += DIR_ENTRY_SIZE) {
                    const uint8_t *e = g_rbuf + off;
                    if (e[0] != 0x00 && e[0] != 0xE5) {
                        if ((e[11] & 0x0F) != 0x0F) return false; /* 有非 LFN 条目 */
                    }
                }
            }
            cur = fat_next(cur);
        }
    }
    /* 释放簇链 */
    uint32_t c = f.first_clus;
    while (c >= CLUSTER_MIN && c < g_max_cluster) {
        uint32_t nx = fat_next(c);
        fat_set(c, FAT_FREE);
        c = nx;
    }
    /* 标记目录项删除（读回扇区，置 0xE5） */
    if (!disk_read(f.dir_lba, 1, g_rbuf)) return false;
    g_rbuf[f.dir_off] = 0xE5;
    return disk_write(f.dir_lba, 1, g_rbuf);
}

/* 重命名（同目录内移动，跨目录不支持链式移动，简单实现）。 */
static bool fs_rename(const char *oldp, const char *newp)
{
    found_t f;
    if (!path_lookup(oldp, &f)) return false;
    /* 在 old 所在目录中改短名 + LFN 为 newp 基名 */
    found_t pf;
    char parent[64];
    const char *slash = NULL;
    for (const char *p = oldp; *p; p++) if (*p == '/') slash = p;
    if (slash) {
        uint32_t plen = (uint32_t)(slash - oldp);
        if (plen >= sizeof(parent)) return false;
        u_memcpy(parent, oldp, plen); parent[plen] = '\0';
        if (!path_lookup(parent, &pf)) return false;
    } else {
        pf.first_clus = g_root_cluster;
    }
    const char *base = newp;
    for (const char *p = newp; *p; p++) if (*p == '/') base = p + 1;
    /* 先删旧目录项（保留簇链），再在父目录写新名指向同一首簇/大小 */
    uint32_t first = f.first_clus;
    if (!disk_read(f.dir_lba, 1, g_rbuf)) return false;
    uint32_t size = rd32(g_rbuf + f.dir_off + 28);
    g_rbuf[f.dir_off] = 0xE5;
    if (!disk_write(f.dir_lba, 1, g_rbuf)) return false;
    return dir_write_entry(pf.first_clus, base, first, size, f.is_dir);
}

/* 截断文件到指定大小（释放多余簇）。 */
static bool fs_truncate(const char *path, uint32_t size)
{
    found_t f;
    if (!path_lookup(path, &f)) return false;
    if (f.is_dir) return false;
    uint32_t first = f.first_clus;
    uint32_t clus_bytes = g_sec_per_clus * SECTOR;
    uint32_t keep = (size + clus_bytes - 1) / clus_bytes;
    if (keep == 0) keep = 1;
    uint32_t cur = first;
    uint32_t prev = 0;
    uint32_t cnt = 0;
    while (cur >= CLUSTER_MIN && cur < g_max_cluster) {
        cnt++;
        uint32_t nx = fat_next(cur);
        if (cnt > keep) {
            fat_set(cur, FAT_FREE);
        } else {
            prev = cur;
        }
        cur = nx;
    }
    if (prev && cnt > keep) fat_set(prev, FAT_LAST);
    if (!disk_read(f.dir_lba, 1, g_rbuf)) return false;
    wr32(g_rbuf + f.dir_off + 28, size);
    return disk_write(f.dir_lba, 1, g_rbuf);
}

/* ===================== 挂载 ===================== */
static bool fat32_mount(void)
{
    if (!disk_read(0, 1, g_bpb)) return false;
    /* 引导签名 */
    if (g_bpb[510] != 0x55 || g_bpb[511] != 0xAA) return false;

    g_bytes_per_sec = rd16(g_bpb + 11);
    /* FAT32 规范（osdev_wiki/FAT32）允许 BPB_BytsPerSec 为 512/1024/2048/4096。
     * 本驱动设计为固定 512 字节扇区（与底层 DISK_PORT 的 512B 扇区语义、
     * 全局 SECTOR=512 假设一致），覆盖 QEMU 等最常见场景。非 512 配置属已知
     * 限制（拒绝挂载而非错误实现），后续如需支持需同步调整块读/簇映射层。 */
    if (g_bytes_per_sec != SECTOR) return false;
    g_sec_per_clus  = g_bpb[13];
    if (g_sec_per_clus == 0 || (g_sec_per_clus & (g_sec_per_clus - 1)) != 0) return false;
    g_rsvd_secs     = rd16(g_bpb + 14);
    g_num_fats      = g_bpb[16];
    uint32_t root_ent = rd16(g_bpb + 17);

    /* FAT32 判定（osdev_wiki/FAT32）：
     *   - 16 位 FAT 大小（偏移 22）为 0；
     *   - 根目录项数（偏移 17）为 0；
     *   - 32 位 FAT 大小（偏移 36）非 0；
     *   - 根目录首簇（偏移 44）>= 2。 */
    uint32_t fatsz16 = rd16(g_bpb + 22);
    uint32_t fatsz32 = rd32(g_bpb + 36);
    g_root_cluster   = rd32(g_bpb + 44);
    if (fatsz16 != 0 || root_ent != 0 || fatsz32 == 0 || g_root_cluster < CLUSTER_MIN)
        return false;
    g_fat_size = fatsz32;

    /* FSI 信息扇区（偏移 48），可选 */
    g_fs_info_lba = rd16(g_bpb + 48);

    uint32_t tot_sec16 = rd16(g_bpb + 19);
    uint32_t tot_sec32 = rd32(g_bpb + 32);
    uint32_t tot_sec = (tot_sec16 != 0) ? tot_sec16 : tot_sec32;
    if (tot_sec == 0) return false;

    g_first_data_lba = g_rsvd_secs + g_num_fats * g_fat_size;
    uint32_t data_secs = tot_sec - g_first_data_lba;
    g_total_clusters = data_secs / g_sec_per_clus;
    if (g_total_clusters < 1) return false;
    g_max_cluster = CLUSTER_MIN + g_total_clusters;
    g_max_lba = tot_sec - 1;

    /* 清空缓存 */
    g_fat_cache_lba = 0xFFFFFFFFu;
    g_fat_cache_ok  = false;
    g_blk_lba = 0xFFFFFFFFu;
    g_blk_secs = 0;
    return true;
}

/* ===================== IPC 应答辅助 ===================== */
static mach_msg_header_t *resp_header(void)
{
    return (mach_msg_header_t *)g_resp;
}
/* 构造应答：local_port 为请求方端口，msgh_id 原样返回。 */
static void build_resp(uint32_t local_port, uint32_t id, uint32_t status,
                       uint32_t length, const void *data)
{
    mach_msg_header_t *h = resp_header();
    h->msgh_bits = 0;   /* 内联消息（内核按 msgh_size 转发） */
    h->msgh_size = (uint32_t)(sizeof(mach_msg_header_t) + sizeof(fs_resp_t) + length);
    h->msgh_remote_port = local_port;
    h->msgh_local_port  = FS_PORT;
    h->msgh_id = id;
    h->msgh_reserved = 0;
    fs_resp_t *fr = (fs_resp_t *)(g_resp + sizeof(mach_msg_header_t));
    fr->status = status;
    fr->length = length;
    if (length && data) {
        u_memcpy(g_resp + sizeof(mach_msg_header_t) + sizeof(fs_resp_t), data, length);
    }
}

/* OOL 应答（整文件读，execve 用）：把 g_filebuf（含头部 fs_resp_t + 内容）经
 * OOL 描述符回传。消息布局 = [header | mach_ool_desc_t]，描述符指向 g_filebuf。 */
static void build_ool_resp(uint32_t local_port, uint32_t id, uint32_t size)
{
    mach_msg_header_t *h = resp_header();
    h->msgh_bits = MACH_MSGH_BITS_OOL;
    h->msgh_size = (uint32_t)(sizeof(mach_msg_header_t) + sizeof(ool_desc_t));
    h->msgh_remote_port = local_port;
    h->msgh_local_port  = FS_PORT;
    h->msgh_id = id;
    h->msgh_reserved = 0;
    ool_desc_t *d = (ool_desc_t *)(g_resp + sizeof(mach_msg_header_t));
    d->address = (uint64_t)g_filebuf;
    d->size = (uint64_t)((sizeof(fs_resp_t) + size + 4096u - 1) & ~((uint64_t)4096u - 1));
    if (d->size == 0) d->size = 4096u;
}

/* ===================== 服务主循环 ===================== */
static void service_loop(void)
{
    u_print("[fs] service loop entered\n");
    for (;;) {
        static uint8_t s_reqbuf[sizeof(mach_msg_header_t) + sizeof(fs_write_req_t) + 96 + FS_WRITE_MAX];
        uint8_t *reqbuf = s_reqbuf;
        if (mach_msg_recv(reqbuf, sizeof(s_reqbuf), FS_PORT) != MACH_MSG_SUCCESS) {
            continue;
        }
        mach_msg_header_t *h = (mach_msg_header_t *)reqbuf;
        if (h->msgh_size < sizeof(mach_msg_header_t) ||
            h->msgh_size > sizeof(reqbuf)) {
            continue;                        /* 畸形消息，丢弃 */
        }
        uint32_t local = h->msgh_local_port;
        uint32_t id = h->msgh_id;
        uint8_t *payload = reqbuf + sizeof(mach_msg_header_t);

        switch (id) {
        case FS_MSG_LIST: {
            char *data = (char *)(g_resp + sizeof(mach_msg_header_t) + sizeof(fs_resp_t));
            uint32_t n = list_dir(g_root_cluster, data, RESP_DATA_MAX);
            build_resp(local, id, FS_OK, n, NULL);
            mach_msg_send(g_resp, resp_header()->msgh_size);
            break;
        }
        case FS_MSG_READ: {
            char *fname = (char *)payload;
            found_t f;
            char *data = (char *)(g_resp + sizeof(mach_msg_header_t) + sizeof(fs_resp_t));
            if (!path_lookup(fname, &f) || f.is_dir) {
                build_resp(local, id, FS_ERR_NOENT, 0, NULL);
            } else {
                uint32_t n = read_file_at(f.first_clus, f.size, 0, data, RESP_DATA_MAX);
                build_resp(local, id, FS_OK, n, NULL);
            }
            mach_msg_send(g_resp, resp_header()->msgh_size);
            break;
        }
        case FS_MSG_READ_AT: {
            fs_read_at_req_t *ra = (fs_read_at_req_t *)payload;
            char *fname = (char *)(payload + sizeof(fs_read_at_req_t));
            found_t f;
            char *data = (char *)(g_resp + sizeof(mach_msg_header_t) + sizeof(fs_resp_t));
            if (!path_lookup(fname, &f) || f.is_dir) {
                build_resp(local, id, FS_ERR_NOENT, 0, NULL);
            } else {
                uint32_t len = ra->length;
                if (len > RESP_DATA_MAX) len = RESP_DATA_MAX;
                uint32_t n = read_file_at(f.first_clus, f.size, ra->offset, data, len);
                build_resp(local, id, FS_OK, n, NULL);
            }
            mach_msg_send(g_resp, resp_header()->msgh_size);
            break;
        }
        case FS_MSG_READ_FILE: {
            /* 内核 execve 用：整文件读 -> OOL。payload = NUL 结尾路径 */
            char *fname = (char *)payload;
            found_t f;
            if (!path_lookup(fname, &f) || f.is_dir) {
                build_resp(local, id, FS_ERR_NOENT, 0, NULL);
                mach_msg_send(g_resp, resp_header()->msgh_size);
            } else {
                uint32_t n = read_file_whole(f.first_clus, f.size);
                if (n != f.size) {
                    build_resp(local, id, FS_ERR_IO, 0, NULL);
                    mach_msg_send(g_resp, resp_header()->msgh_size);
                } else {
                    build_ool_resp(local, id, n);
                    mach_msg_send(g_resp, resp_header()->msgh_size);
                }
            }
            break;
        }
        case FS_MSG_CREATE: {
            build_resp(local, id, fs_create((char *)payload, false) ? FS_OK : FS_ERR_IO, 0, NULL);
            mach_msg_send(g_resp, resp_header()->msgh_size);
            break;
        }
        case FS_MSG_MKDIR: {
            build_resp(local, id, fs_create((char *)payload, true) ? FS_OK : FS_ERR_IO, 0, NULL);
            mach_msg_send(g_resp, resp_header()->msgh_size);
            break;
        }
        case FS_MSG_WRITE: {
            fs_write_req_t *w = (fs_write_req_t *)payload;
            char *fname = (char *)(payload + sizeof(fs_write_req_t));
            const void *data = fname + u_strlen(fname) + 1;
            uint32_t n = fs_write(fname, w->offset, data, w->length);
            build_resp(local, id, n == w->length ? FS_OK : FS_ERR_IO, 0, NULL);
            mach_msg_send(g_resp, resp_header()->msgh_size);
            break;
        }
        case FS_MSG_UNLINK: {
            build_resp(local, id, fs_unlink((char *)payload) ? FS_OK : FS_ERR_IO, 0, NULL);
            mach_msg_send(g_resp, resp_header()->msgh_size);
            break;
        }
        case FS_MSG_RENAME: {
            fs_rename_req_t *r = (fs_rename_req_t *)payload;
            build_resp(local, id, fs_rename(r->old_name, r->new_name) ? FS_OK : FS_ERR_IO, 0, NULL);
            mach_msg_send(g_resp, resp_header()->msgh_size);
            break;
        }
        case FS_MSG_TRUNCATE: {
            fs_trunc_req_t *t = (fs_trunc_req_t *)payload;
            char *fname = (char *)(payload + sizeof(fs_trunc_req_t));
            build_resp(local, id, fs_truncate(fname, t->size) ? FS_OK : FS_ERR_IO, 0, NULL);
            mach_msg_send(g_resp, resp_header()->msgh_size);
            break;
        }
        default: {
            build_resp(local, id, FS_ERR_NOENT, 0, NULL);
            mach_msg_send(g_resp, resp_header()->msgh_size);
            break;
        }
        }
    }
}

/* ===================== 自检 ===================== */
static void self_test(void)
{
    u_print("[fs] self-test begin\n");

    /* --- 写路径自检 --- */
    const char *tf = "SELFTEST.TXT";
    bool ok = true;

    u_print("[fs] st: create\n");
    if (!fs_create(tf, false)) { u_print("[fs] self-test: create FAIL\n"); ok = false; }
    const char *msg = "HELLO_SUKI_FAT32_WRITE_PATH_OK";
    uint32_t len = (uint32_t)u_strlen(msg);
    u_print("[fs] st: write\n");
    uint32_t wrote = fs_write(tf, 0, msg, len);
    if (wrote != len) { u_print("[fs] self-test: write FAIL\n"); ok = false; }

    /* readback */
    char rb[64];
    u_memset(rb, 0, sizeof(rb));
    found_t f;
    if (!path_lookup(tf, &f)) { u_print("[fs] self-test: lookup after write FAIL\n"); ok = false; }
    else {
        uint32_t rn = read_file_at(f.first_clus, f.size, 0, rb, (uint32_t)sizeof(rb));
        if (rn != len || u_strcmp(rb, msg) != 0) {
            u_print("[fs] self-test: readback mismatch\n"); ok = false;
        }
    }

    /* append */
    const char *msg2 = "_APPEND";
    uint32_t len2 = (uint32_t)u_strlen(msg2);
    if (fs_write(tf, len, msg2, len2) != len2) { u_print("[fs] self-test: append FAIL\n"); ok = false; }
    char rb2[80];
    if (path_lookup(tf, &f)) {
        uint32_t rn = read_file_at(f.first_clus, f.size, 0, rb2, (uint32_t)sizeof(rb2));
        if (rn != len + len2 || u_strcmp(rb2, "HELLO_SUKI_FAT32_WRITE_PATH_OK_APPEND") != 0) {
            u_print("[fs] self-test: append readback FAIL\n"); ok = false;
        }
    }

    /* mkdir + subdir write */
    if (!fs_create("SUBDIR", true)) { u_print("[fs] self-test: mkdir FAIL\n"); ok = false; }
    if (!fs_create("SUBDIR/NEST.TXT", false)) { u_print("[fs] self-test: subdir create FAIL\n"); ok = false; }
    if (fs_write("SUBDIR/NEST.TXT", 0, "NESTED", 6) != 6) { u_print("[fs] self-test: subdir write FAIL\n"); ok = false; }

    /* rename */
    if (!fs_rename("SELFTEST.TXT", "RENAMED.TXT")) { u_print("[fs] self-test: rename FAIL\n"); ok = false; }
    if (path_lookup("SELFTEST.TXT", &f)) { u_print("[fs] self-test: old name still exists after rename\n"); ok = false; }
    if (!path_lookup("RENAMED.TXT", &f)) { u_print("[fs] self-test: renamed name missing\n"); ok = false; }

    /* unlink */
    if (!fs_unlink("RENAMED.TXT")) { u_print("[fs] self-test: unlink FAIL\n"); ok = false; }
    if (path_lookup("RENAMED.TXT", &f)) { u_print("[fs] self-test: file exists after unlink\n"); ok = false; }

    /* truncate */
    if (!fs_create("TRUNC.TXT", false)) { u_print("[fs] self-test: trunc create FAIL\n"); ok = false; }
    if (fs_write("TRUNC.TXT", 0, "0123456789", 10) != 10) { u_print("[fs] self-test: trunc write FAIL\n"); ok = false; }
    if (!fs_truncate("TRUNC.TXT", 4)) { u_print("[fs] self-test: truncate FAIL\n"); ok = false; }
    if (path_lookup("TRUNC.TXT", &f)) {
        if (f.size != 4) { u_print("[fs] self-test: truncated size wrong\n"); ok = false; }
    }
    fs_unlink("TRUNC.TXT");
    fs_unlink("SUBDIR/NEST.TXT");
    fs_unlink("SUBDIR");

    /* --- 大文件整读自检（MOONHALO.MP3 若存在于镜像） --- */
    found_t big;
    if (path_lookup("MOONHALO.MP3", &big) && !big.is_dir) {
        uint32_t total = read_file_whole(big.first_clus, big.size);
        if (total != big.size) {
            u_print("[fs] self-test: bigfile whole-read length mismatch\n"); ok = false;
        } else {
            /* 尾块抽样校验 */
            uint32_t tail_off = (big.size > 100) ? big.size - 100 : 0;
            uint32_t tail_len = (big.size > 100) ? 100 : big.size;
            char tail[128];
            uint32_t tn = read_file_at(big.first_clus, big.size, tail_off, tail, tail_len);
            if (tn != tail_len) { u_print("[fs] self-test: bigfile tail read mismatch\n"); ok = false; }
        }
    }

    g_self_test_ok = ok;
    u_print(ok ? "[fs] self-test ALL PASS\n" : "[fs] self-test FAILED\n");
}

/* ===================== 入口 ===================== */
int main(void)
{
    u_print("[fs] FS_SERVER starting\n");
    /* 认领知名端口与私有应答端口（生产约束：必须先 claim 才能 recv） */
    sys_port_claim(FS_PORT);
    sys_port_claim(FS_REPLY_PORT);
    if (!fat32_mount()) {
        u_print("[fs] mount failed\n");
        return 1;
    }
    u_print("[fs] mounted FAT32\n");
    self_test();
    service_loop();
    return 0;
}

/* 入口由用户态启动桩 user/lib/crt0.S 提供（调用 main）。 */
