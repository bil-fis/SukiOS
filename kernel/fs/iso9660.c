/*
 * kernel/fs/iso9660.c
 * -----------------------------------------------------------------------------
 * 内核内嵌 ISO9660 文件系统驱动（只读，支持 Rock Ridge 与 Joliet 扩展）。
 *
 * 职责边界（OSDev《ISO 9660》《Rock Ridge》《Joliet》）：
 *   - 块 I/O 全部经 kernel/drivers/cdrom.c 的 CdromReadBlocks()（ATAPI 2048 字节块）；
 *   - 本文件解析卷描述符(PVD/SVD)、目录记录(Directory Record)、路径表替代为
 *     目录树递归 namei；提供 open/read/stat/opendir/readdir/closedir 句柄式 API，
 *     供内核 VFS 层(fd.c/vfs.c)把 '/' 直接路由到光盘，从而脱离硬盘启动。
 *
 * 文件名解析优先级（每个目录记录）：
 *   1) 若走 Rock Ridge 主树（g_use_joliet==false）且系统使用区有 "NM"(CURRENT)
 *      则采用其真实文件名；否则用 ISO 标准 8.3 名（去 ";版本号"）。
 *   2) 若走 Joliet 树（g_use_joliet==true）则把记录内 UCS-2(BE) 文件名解码为 UTF-8。
 *   权限：Rock Ridge "PX" 提供完整 POSIX mode（含 IFREG/IFDIR 位）；否则按
 *   目录/文件位给出 0555/0444 默认权限。
 *
 * 红线：只读。所有写类操作在 fd.c 层被拒（-EROFS），本文件不提供任何写路径。
 */
#include <kernel/iso9660.h>
#include <kernel/cdrom.h>
#include <kernel/string.h>
#include <kernel/console.h>
#include <mm/kmalloc.h>
#include <ipc/fs_proto.h>
#include <sukios/posix.h>      /* SUKI_S_IF* / SUKI_DT_* / SUKI_E* / SUKI_SEEK_* */

#define ISO_BLOCK       2048u
#define ISO_PVD_LBA     16u     /* 卷描述符从 LBA 16 起 */
#define ISO_MAX_FH      64
#define ISO_MAX_DIRSEC  512     /* 单目录上限 512 个逻辑块（=1MiB 目录） */

/* ============================ 全局状态 ============================ */
static bool     g_iso_mounted = false;
static bool     g_use_joliet  = false;   /* true: 走 Joliet 目录树(UCS-2 名) */
static bool     g_rockridge  = false;    /* 主树含 Rock Ridge（SP 生效） */
static uint32_t g_root_lba   = 0;
static uint64_t g_root_size  = 0;

/* ============================ 小工具 ============================ */
static inline uint32_t Le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 大小写无关比较：相等返回 0（ISO9660 文件名大小写不敏感，便于匹配请求路径） */
static int StriCmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) {
            return (int)ca - (int)cb;
        }
        a++; b++;
    }
    return (int)(*a) - (int)(*b);
}

/* 从 extent 内字节偏移 off 读 len 字节到 out（按 2048 块对齐读，按需截取）。 */
static int IsoReadExtent(uint32_t first_lba, uint64_t off, uint32_t len, uint8_t *out)
{
    if (len == 0) {
        return 0;
    }
    uint64_t start = off;
    uint64_t end   = off + len;
    uint32_t first = (uint32_t)(start / ISO_BLOCK);
    uint32_t last  = (uint32_t)((end + ISO_BLOCK - 1) / ISO_BLOCK);
    if (last < first) {
        return -1;
    }
    uint32_t nsec = last - first + 1;
    if (nsec > 256) {
        return -1;     /* 防御：单次读取过大 */
    }
    uint8_t *tmp = kmalloc(nsec * ISO_BLOCK);
    if (!tmp) {
        return -1;
    }
    if (!CdromReadBlocks(first_lba + first, nsec, tmp)) {
        kfree(tmp);
        return -1;
    }
    memcpy(out, tmp + (uint32_t)(start % ISO_BLOCK), len);
    kfree(tmp);
    return 0;
}

/* SUSP 系统使用区扫描：查找 2 字节签名 sig(大端) 的条目，返回 true。 */
static bool ScanSig(const uint8_t *p, uint32_t len, uint16_t sig)
{
    uint32_t off = 0;
    while (off + 4 <= len) {
        uint16_t s = (uint16_t)((p[off] << 8) | p[off + 1]);
        uint8_t  elen = p[off + 2];
        if (elen < 4 || off + elen > len) {
            break;
        }
        if (s == sig) {
            return true;
        }
        off += elen;
    }
    return false;
}

/* Rock Ridge "NM"：拼装真实文件名（CURRENT 标志表示终名，CONTINUE 表示续接）。 */
static bool RockRidgeName(const uint8_t *su, uint32_t su_len, char *out)
{
    char    accum[256];
    uint32_t accum_len = 0;
    uint32_t off = 0;
    bool    got = false;
    while (off + 4 <= su_len) {
        uint16_t s = (uint16_t)((su[off] << 8) | su[off + 1]);
        uint8_t  elen = su[off + 2];
        if (elen < 4 || off + elen > su_len) {
            break;
        }
        if (s == 0x4E4D) {                 /* "NM" */
            /* Rock Ridge NM 记录布局：sig(2) len(1) version(1) flags(1) name(len-5)
             * 注意：flags 在 off+4，真实文件名从 off+5 起，长度为 elen-5。 */
            uint8_t flags = su[off + 4];
            const uint8_t *nm = su + off + 5;
            uint8_t nmlen = (uint8_t)(elen - 5);
            if (accum_len + nmlen < 255) {
                memcpy(accum + accum_len, nm, nmlen);
                accum_len += nmlen;
            }
            if (flags & 0x02) {            /* CURRENT：终名 */
                accum[accum_len] = '\0';
                memcpy(out, accum, accum_len + 1);
                got = true;
            }
        }
        off += elen;
    }
    if (got) {
        return true;
    }
    if (accum_len > 0) {
        accum[accum_len] = '\0';
        memcpy(out, accum, accum_len + 1);
        return true;
    }
    return false;
}

/* Rock Ridge "PX"：取 POSIX mode（含 IFREG/IFDIR 与权限位）。 */
static bool RockRidgeMode(const uint8_t *su, uint32_t su_len, uint32_t *mode)
{
    uint32_t off = 0;
    while (off + 4 <= su_len) {
        uint16_t s = (uint16_t)((su[off] << 8) | su[off + 1]);
        uint8_t  elen = su[off + 2];
        if (elen < 4 || off + elen > su_len) {
            break;
        }
        if (s == 0x5058) {                 /* "PX" */
            if (elen >= 12) {
                *mode = Le32(su + off + 4);
                return true;
            }
        }
        off += elen;
    }
    return false;
}

/* Joliet UCS-2(BE) -> UTF-8 解码，并去掉 ";版本号" 与尾部空格。 */
static void DecodeJoliet(const uint8_t *id, uint8_t idlen, char *out)
{
    uint32_t o = 0;
    for (uint32_t i = 0; i + 1 < idlen; i += 2) {
        uint32_t c = ((uint32_t)id[i] << 8) | id[i + 1];
        if (c == 0) {
            break;
        }
        if (c < 0x80) {
            if (o + 1 < 256) out[o++] = (char)c;
        } else if (c < 0x800) {
            if (o + 2 < 256) {
                out[o++] = (char)(0xC0 | (c >> 6));
                out[o++] = (char)(0x80 | (c & 0x3F));
            }
        } else {
            if (o + 3 < 256) {
                out[o++] = (char)(0xE0 | (c >> 12));
                out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
                out[o++] = (char)(0x80 | (c & 0x3F));
            }
        }
    }
    out[o] = '\0';
    char *sem = strchr(out, ';');
    if (sem) {
        *sem = '\0';
    }
    while (o > 0 && out[o - 1] == ' ') {
        out[--o] = '\0';
    }
}

/* ISO 标准 8.3 名：直接拷贝并去掉 ";N" 版本号。 */
static void PlainName(const uint8_t *id, uint8_t idlen, char *out)
{
    uint32_t n = idlen < 255 ? (uint32_t)idlen : 255u;
    memcpy(out, id, n);
    out[n] = '\0';
    char *sem = strchr(out, ';');
    if (sem) {
        *sem = '\0';
    }
}

/* ============================ 目录项解析 ============================ */
typedef struct {
    bool     is_dir;
    uint32_t extent_lba;
    uint64_t size;
    uint32_t mode;        /* POSIX mode（含 IFREG/IFDIR 位） */
} iso_node_t;

/* 解析一条目录记录：填 name_out 与 node。rec_off 为记录在目录内的字节偏移
 * （用于合成 Rock Ridge 的 '.'/'..' 合成名）。返回记录长度(>0) 或 0（无效）。 */
static uint8_t IsoParseRecord(const uint8_t *rec, uint32_t rec_off, char *name_out, iso_node_t *node)
{
    uint8_t reclen = rec[0];
    if (reclen < 33) {
        return 0;
    }
    uint32_t extent = Le32(rec + 2);
    uint64_t size   = (uint64_t)Le32(rec + 10);
    uint8_t  flags  = rec[25];
    uint8_t  idlen  = rec[32];
    const uint8_t *id = rec + 33;
    /* 系统使用区起点。注意：grub-mkrescue/genisoimage 生成的 Rock Ridge ISO
     * 文件名段不补奇偶 padding（与 ISO9660 规范"补到偶长度"有出入），故直接用
     * 33 + idlen；偏移错 1 字节会导致 RockRidgeName 整体错位、找不到 NM。 */
    uint32_t su    = 33u + idlen;
    uint32_t su_len = reclen - su;

    if (g_use_joliet) {
        DecodeJoliet(id, idlen, name_out);
    } else {
        if (!RockRidgeName(rec + su, su_len, name_out)) {
            PlainName(id, idlen, name_out);
        }
    }

    /* Rock Ridge 的 '.'/'..' 目录记录：标准标识符为 NUL 字节且不带 NM 系统使用项，
     * 名字须按 ISO9660 规范合成——首条(offset==0)为 '.'，第二条为 '..'。 */
    if (name_out[0] == '\0') {
        if (rec_off == 0) {
            strcpy(name_out, ".");
        } else {
            strcpy(name_out, "..");
        }
    }

    node->is_dir     = (flags & 0x02) != 0;
    node->extent_lba = extent;
    node->size       = size;

    uint32_t mode = 0;
    if (!g_use_joliet && RockRidgeMode(rec + su, su_len, &mode)) {
        node->mode = mode;
    } else {
        node->mode = (flags & 0x02)
                     ? (uint32_t)(SUKI_S_IFDIR  | 0555)
                     : (uint32_t)(SUKI_S_IFREG  | 0444);
    }
    return reclen;
}

/* 在目录 extent 中按名查找子项（大小写无关），命中填 *out 返回 0。 */
static int IsoDirLookup(uint32_t dir_lba, uint64_t dir_size,
                        const char *name, iso_node_t *out)
{
    uint32_t nsec = (uint32_t)((dir_size + ISO_BLOCK - 1) / ISO_BLOCK);
    if (nsec == 0 || nsec > ISO_MAX_DIRSEC) {
        return -1;
    }
    uint8_t *buf = kmalloc(nsec * ISO_BLOCK);
    if (!buf) {
        return -1;
    }
    if (!CdromReadBlocks(dir_lba, nsec, buf)) {
        kfree(buf);
        return -1;
    }
    uint64_t off = 0;
    int rc = -1;
    while (off < dir_size) {
        const uint8_t *rec = buf + off;
        uint8_t reclen = rec[0];
        if (reclen == 0) {
            /* 本扇区剩余为填充，跳到下一个逻辑块边界 */
            off = (off + ISO_BLOCK) & ~((uint64_t)ISO_BLOCK - 1);
            continue;
        }
        char     nm[256];
        iso_node_t nd;
        uint8_t rl = IsoParseRecord(rec, (uint32_t)off, nm, &nd);
        if (rl < 33) {
            off += reclen;
            continue;
        }
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) {
            off += rl;
            continue;
        }
        if (StriCmp(nm, name) == 0) {
            *out = nd;
            rc = 0;
            break;
        }
        off += rl;
    }
    kfree(buf);
    return rc;
}

/* 路径解析（namei）：从根目录递归按 '/' 分量下降。 */
static int IsoResolve(const char *path, iso_node_t *out)
{
    iso_node_t cur;
    cur.is_dir     = true;
    cur.extent_lba = g_root_lba;
    cur.size       = g_root_size;
    cur.mode       = (uint32_t)(SUKI_S_IFDIR | 0555);

    const char *p = path;
    if (*p == '/') {
        p++;
    }
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t clen = slash ? (size_t)(slash - p) : strlen(p);
        if (clen == 0) {                 /* 空分量（如 "//"） */
            p++;
            continue;
        }
        if (clen > 255) {
            clen = 255;
        }
        char comp[256];
        memcpy(comp, p, clen);
        comp[clen] = '\0';

        if (!cur.is_dir) {
            return -1;                   /* 路径中间分量不是目录 */
        }
        iso_node_t child;
        if (IsoDirLookup(cur.extent_lba, cur.size, comp, &child) != 0) {
            return -1;
        }
        cur = child;
        p = slash ? slash + 1 : p + clen;
    }
    *out = cur;
    return 0;
}

/* ============================ 挂载 ============================ */
/* 检测主树根目录第一个记录（"."）系统使用区是否含 Rock Ridge "SP"。 */
static bool HasRockRidge(uint32_t dir_lba)
{
    uint8_t *sec = kmalloc(ISO_BLOCK);
    if (!sec) {
        return false;
    }
    if (!CdromReadBlocks(dir_lba, 1, sec)) {
        kfree(sec);
        return false;
    }
    bool found = false;
    uint8_t reclen = sec[0];
    if (reclen >= 34) {
        uint8_t idlen = sec[32];
        uint32_t su = 33u + idlen;   /* 与解析器一致的 RR 偏移（无奇偶 padding） */
        uint32_t su_len = reclen - su;
        if (ScanSig(sec + su, su_len, 0x5350)) {   /* "SP" */
            found = true;
        }
    }
    kfree(sec);
    return found;
}

bool IsoMount(void)
{
    if (!CdromPresent()) {
        return false;
    }
    uint8_t *vd = kmalloc(ISO_BLOCK);
    if (!vd) {
        return false;
    }
    if (!CdromReadBlocks(ISO_PVD_LBA, 1, vd)) {
        kfree(vd);
        return false;
    }
    /* 主卷描述符：type==1, id=="CD001" */
    if (vd[0] != 1 || memcmp(vd + 1, "CD001", 5) != 0) {
        kfree(vd);
        return false;
    }
    const uint8_t *rr = vd + 156;        /* 根目录记录（34 字节） */
    if (rr[0] < 34) {
        kfree(vd);
        return false;
    }
    g_root_lba  = Le32(rr + 2);
    g_root_size = (uint64_t)Le32(rr + 10);

    /* 扫描后续卷描述符：找 Joliet 补充卷(SVD, type=2)。 */
    bool     joliet_found   = false;
    uint32_t joliet_root_lba = 0;
    uint64_t joliet_root_size = 0;
    for (int i = 1; i < 32; i++) {
        uint32_t lba = ISO_PVD_LBA + (uint32_t)i;
        if (!CdromReadBlocks(lba, 1, vd)) {
            break;
        }
        uint8_t type = vd[0];
        if (type == 0) {
            break;                       /* 终止符 */
        }
        if (type == 2) {                 /* Supplementary Volume Descriptor */
            /* 转义序列位于 offset 88（32 字节），Joliet 以 %/@、%/E、%/C 标识 */
            const uint8_t *esc = vd + 88;
            bool joliet = false;
            for (int e = 0; e + 3 <= 32; e++) {
                if (esc[e] == 0x25 && esc[e + 1] == 0x2F &&
                    (esc[e + 2] == 0x40 || esc[e + 2] == 0x45 ||
                     esc[e + 2] == 0x43)) {
                    joliet = true;
                    break;
                }
            }
            if (joliet) {
                const uint8_t *jr = vd + 156;
                if (jr[0] >= 34) {
                    joliet_root_lba   = Le32(jr + 2);
                    joliet_root_size  = (uint64_t)Le32(jr + 10);
                    joliet_found = true;
                }
            }
        }
    }

    g_rockridge = HasRockRidge(g_root_lba);
    if (joliet_found && !g_rockridge) {
        /* 没有 Rock Ridge 时，自动启用 Joliet 长名树 */
        g_use_joliet = true;
        g_root_lba   = joliet_root_lba;
        g_root_size  = joliet_root_size;
    } else {
        g_use_joliet = false;
    }

    kfree(vd);
    g_iso_mounted = true;
    kprintf("[iso] mounted: rockridge=%u joliet=%u root_lba=%u root_size=%llu\n",
            (unsigned)g_rockridge, (unsigned)g_use_joliet, (unsigned)g_root_lba,
            (unsigned long long)g_root_size);
    return true;
}

bool IsoIsMounted(void)
{
    return g_iso_mounted;
}

/* ============================ 句柄表 ============================ */
typedef struct {
    bool     used;
    bool     is_dir;
    uint32_t extent_lba;
    uint64_t size;
    uint64_t pos;            /* 文件读偏移 */
    uint8_t *dir_buf;        /* opendir 时整目录载入内核缓冲 */
    uint32_t dir_size;
    uint32_t dir_off;        /* readdir 扫描字节偏移（目录内） */
} iso_fh_t;

static iso_fh_t g_fh[ISO_MAX_FH];

static int IsoAllocHandle(void)
{
    for (int i = 0; i < ISO_MAX_FH; i++) {
        if (!g_fh[i].used) {
            return i;
        }
    }
    return -1;
}

int iso_open(const char *path, int32_t flags, uint32_t mode, uint32_t *out_handle)
{
    (void)flags;
    (void)mode;
    if (!g_iso_mounted) {
        return -SUKI_EIO;
    }
    iso_node_t node;
    if (IsoResolve(path, &node) != 0) {
        return -SUKI_ENOENT;
    }
    int slot = IsoAllocHandle();
    if (slot < 0) {
        return -SUKI_ENFILE;
    }
    iso_fh_t *f = &g_fh[slot];
    memset(f, 0, sizeof(*f));
    f->used       = true;
    f->is_dir     = node.is_dir;
    f->extent_lba = node.extent_lba;
    f->size       = node.size;
    f->pos        = 0;

    if (node.is_dir) {
        uint32_t nsec = (uint32_t)((node.size + ISO_BLOCK - 1) / ISO_BLOCK);
        if (nsec == 0 || nsec > ISO_MAX_DIRSEC) {
            f->used = false;
            return -SUKI_EIO;
        }
        f->dir_buf = kmalloc(nsec * ISO_BLOCK);
        if (!f->dir_buf) {
            f->used = false;
            return -SUKI_ENOMEM;
        }
        if (!CdromReadBlocks(node.extent_lba, nsec, f->dir_buf)) {
            kfree(f->dir_buf);
            f->dir_buf = NULL;
            f->used = false;
            return -SUKI_EIO;
        }
        f->dir_size = (uint32_t)node.size;
    }

    *out_handle = (uint32_t)slot;
    return 0;
}

int iso_read(uint32_t h, void *buf, uint32_t len, uint64_t *out_nread)
{
    if (h >= ISO_MAX_FH) {
        return -SUKI_EBADF;
    }
    iso_fh_t *f = &g_fh[h];
    if (!f->used || f->is_dir) {
        return -SUKI_EBADF;
    }
    if (f->pos >= f->size) {
        *out_nread = 0;
        return 0;
    }
    uint64_t rem = f->size - f->pos;
    uint32_t clen = (len > rem) ? (uint32_t)rem : len;
    if (clen == 0) {
        *out_nread = 0;
        return 0;
    }
    if (IsoReadExtent(f->extent_lba, f->pos, clen, (uint8_t *)buf) != 0) {
        return -SUKI_EIO;
    }
    f->pos += clen;
    *out_nread = clen;
    return 0;
}

int iso_lseek(uint32_t h, int64_t offset, int whence, uint64_t *out_pos)
{
    if (h >= ISO_MAX_FH) {
        return -SUKI_EBADF;
    }
    iso_fh_t *f = &g_fh[h];
    if (!f->used) {
        return -SUKI_EBADF;
    }
    uint64_t base;
    if (whence == SUKI_SEEK_SET) {
        base = 0;
    } else if (whence == SUKI_SEEK_CUR) {
        base = f->pos;
    } else if (whence == SUKI_SEEK_END) {
        base = f->size;
    } else {
        return -SUKI_EINVAL;
    }
    int64_t np = (int64_t)base + offset;
    if (np < 0) {
        np = 0;
    }
    if ((uint64_t)np > f->size) {
        np = (int64_t)f->size;
    }
    f->pos = (uint64_t)np;
    *out_pos = f->pos;
    return 0;
}

int iso_close(uint32_t h)
{
    if (h >= ISO_MAX_FH) {
        return -SUKI_EBADF;
    }
    iso_fh_t *f = &g_fh[h];
    if (!f->used) {
        return -SUKI_EBADF;
    }
    if (f->dir_buf) {
        kfree(f->dir_buf);
        f->dir_buf = NULL;
    }
    f->used = false;
    return 0;
}

int iso_stat(const char *path, fs_stat_t *st)
{
    if (!g_iso_mounted) {
        return -SUKI_EIO;
    }
    iso_node_t node;
    if (IsoResolve(path, &node) != 0) {
        return -SUKI_ENOENT;
    }
    memset(st, 0, sizeof(*st));
    st->mode    = node.mode | (node.is_dir ? SUKI_S_IFDIR : SUKI_S_IFREG);
    st->size    = (int64_t)node.size;
    st->blksize = (int64_t)ISO_BLOCK;
    st->blocks  = (int64_t)((node.size + ISO_BLOCK - 1) / ISO_BLOCK);
    st->nlink   = 1;
    st->ino     = (uint64_t)node.extent_lba;
    return 0;
}

int iso_opendir(const char *path)
{
    if (!g_iso_mounted) {
        return -SUKI_EIO;
    }
    iso_node_t node;
    if (IsoResolve(path, &node) != 0) {
        return -SUKI_ENOENT;
    }
    if (!node.is_dir) {
        return -SUKI_ENOTDIR;
    }
    uint32_t h;
    int rc = iso_open(path, 0, 0, &h);
    if (rc < 0) {
        return rc;
    }
    return (int)h;
}

int iso_readdir(int dd, fs_dirent_t *de)
{
    if (dd >= ISO_MAX_FH) {
        return -SUKI_EBADF;
    }
    iso_fh_t *f = &g_fh[dd];
    if (!f->used || !f->is_dir || !f->dir_buf) {
        return -SUKI_EBADF;
    }
    while (f->dir_off < f->dir_size) {
        const uint8_t *rec = f->dir_buf + f->dir_off;
        uint8_t reclen = rec[0];
        if (reclen == 0) {
            f->dir_off = (f->dir_off + ISO_BLOCK) & ~((uint32_t)ISO_BLOCK - 1);
            continue;
        }
        char     nm[256];
        iso_node_t nd;
        uint8_t rl = IsoParseRecord(rec, f->dir_off, nm, &nd);
        if (rl < 33) {
            f->dir_off += reclen;
            continue;
        }
        f->dir_off += rl;
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) {
            continue;
        }
        memset(de, 0, sizeof(*de));
        de->ino  = (uint64_t)nd.extent_lba;
        de->type = nd.is_dir ? SUKI_DT_DIR : SUKI_DT_REG;
        strncpy(de->name, nm, sizeof(de->name) - 1);
        return 1;                        /* 有条目 */
    }
    return 0;                           /* 目录结束 */
}

int iso_closedir(int dd)
{
    return iso_close((uint32_t)dd);
}

/* ============================ 内核态整文件读 ============================ */
int IsoReadFile(const char *path, uint8_t *buf, uint32_t cap, uint32_t *out_n)
{
    if (!g_iso_mounted) {
        return -SUKI_EIO;
    }
    uint32_t h;
    if (iso_open(path, 0, 0, &h) != 0) {
        return -SUKI_ENOENT;
    }
    uint64_t total = 0;
    uint64_t nr    = 0;
    uint8_t tmp[512];
    while (total < cap) {
        uint32_t chunk = (uint32_t)(cap - total);
        if (chunk > 512) {
            chunk = 512;
        }
        int rc = iso_read(h, tmp, chunk, &nr);
        if (rc < 0) {
            iso_close(h);
            return rc;
        }
        if (nr == 0) {
            break;
        }
        memcpy(buf + total, tmp, (uint32_t)nr);
        total += nr;
    }
    iso_close(h);
    if (out_n) {
        *out_n = (uint32_t)total;
    }
    return 0;
}

/* ============================ 自测 ============================ */
void IsoSelfTest(void)
{
    if (!g_iso_mounted) {
        return;
    }
    /* 读一个已知小文件（ISO 根目录下的 README.TXT），回环验证全链路 */
    uint8_t buf[256];
    uint32_t n = 0;
    int rc = IsoReadFile("/README.TXT", buf, sizeof(buf) - 1, &n);
    if (rc == 0 && n > 0) {
        buf[n] = '\0';
        kprintf("[iso] selftest PASS: /README.TXT %u bytes: '%.*s'\n",
                n, (int)n, (char *)buf);
    } else {
        kprintf("[iso] selftest: /README.TXT rc=%d (absent or unreadable)\n", rc);
    }
    /* 列出根目录条目（验证 namei + readdir） */
    int dd = iso_opendir("/");
    if (dd >= 0) {
        fs_dirent_t de;
        kprintf("[iso] root entries: ");
        int cnt = 0;
        while (iso_readdir((uint32_t)dd, &de) == 1) {
            kprintf("%s ", de.name);
            cnt++;
        }
        kprintf("(%d total)\n", cnt);
        iso_closedir((uint32_t)dd);
    }
}
