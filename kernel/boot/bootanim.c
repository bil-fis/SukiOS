/*
 * kernel/boot/bootanim.c
 * -----------------------------------------------------------------------------
 * 启动动画实现：启动图标（内嵌 bmp / 自定义 bmp）+ 圆角进度条。
 *
 * 设计要点：
 *   1) 图标来源三选一（优先级从高到低）：
 *        a. CustomLogo/Enabled=true -> CustomLogo/DirectPath（指定驱动器直读）
 *           或 CustomLogo/Path（走 VFS / 内核 ISO9660 回退）；
 *        b. BootLogoID 指定的【内嵌】启动图（boot/anim 下的 .bmp 构建期转码链入）；
 *        c. BootLogoID 为空/未命中时，回退到内嵌表的第一张（便于「只丢 bmp」即生效）。
 *   2) 进度条：黑底圆角轨道 + 白色圆角填充（两侧圆角即药丸形），
 *      随 boot_late_init 的各里程碑推进（见 bootanim_progress）。
 *   3) 全部为整数运算（内核 -mgeneral-regs-only，无 FPU/SSE）；
 *      缩放用 16.16 定点最近邻，仅在图标大于目标框时缩小。
 *   4) 任何一步失败都只打印一行日志并继续，绝不阻塞启动。
 */
#include <kernel/bootanim.h>
#include <kernel/framebuffer.h>
#include <kernel/console.h>
#include <kernel/string.h>
#include <kernel/clock.h>      /* clock_monotonic_ns：最短展示时长计时 */
#include <kernel/task.h>       /* msleep：让出 CPU 的毫秒睡眠（不忙等） */
#include <kernel/vfs.h>        /* kern_fs_read_file */
#include <kernel/iso9660.h>    /* IsoReadFile / IsoIsMounted */
#include <mm/kmalloc.h>

/* 自定义 logo 上限：文件原始字节与解码后像素缓冲各 16 MiB（一次性、用完即释放）。
 * 超出视为配置错误，回退内嵌启动图，绝不因此阻塞启动。 */
#define ANIM_CUSTOM_MAX  (16u * 1024u * 1024u)

/* 启动画面最短可见时长（ms）。单核下从 stage3 绘制启动画面到显示服务接管
 * 帧缓冲区往往不足 0.5 秒（实测连拍只有 1 帧），进度条会「一闪而过」而失去意义。
 * 故 bootanim_finish() 保证至少显示这么久，期间把进度平滑补到 100% 再返回。
 * 纯文本模式（无帧缓冲）不生效，绝不因此阻塞启动。 */
#define BOOTANIM_MIN_MS  2000u

static bool     g_anim_ready = false;   /* bootanim_setup 是否成功初始化 */
static volatile bool g_boot_seq_done = false;  /* 启动序列完成（放行显示服务） */
static bool     g_anim_show_progress = false;
static uint32_t g_anim_pct = 0;         /* 当前进度（单调不减） */
static uint64_t g_anim_t0_ns = 0;       /* 启动画面绘制时刻（最短展示时长计时基准） */

/* 进度条几何（setup 时按屏幕尺寸算出） */
static uint32_t g_bar_x, g_bar_y, g_bar_w, g_bar_h, g_bar_r;

/* ------------------------------------------------------------------ 小工具 */

static uint32_t AnimLe16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t AnimLe32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 小整数平方根（仅用于圆角半径 <= 32，逐次试探足够快） */
static uint32_t AnimIsqrt(uint32_t v)
{
    uint32_t r = 0;
    while ((r + 1) * (r + 1) <= v)
        r++;
    return r;
}

/* ------------------------------------------------------------------ 绘制 */

/*
 * AnimRoundRect —— 圆角矩形填充（两侧圆角）。
 * r=0 时退化为普通矩形；r=h/2 时为药丸形（进度条两端圆角）。
 */
static void AnimRoundRect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                          uint32_t r, uint32_t color)
{
    if (w == 0 || h == 0)
        return;
    if (r > h / 2) r = h / 2;
    if (r > w / 2) r = w / 2;
    for (uint32_t ry = 0; ry < h; ry++) {
        uint32_t inset = 0;
        if (r > 0) {
            if (ry < r) {
                uint32_t dy = r - ry;
                inset = r - AnimIsqrt(r * r - dy * dy);
            } else if (ry >= h - r) {
                uint32_t dy = ry - (h - r) + 1;
                inset = r - AnimIsqrt(r * r - dy * dy);
            }
        }
        if (inset * 2 >= w)
            continue;
        fb_fill_rect(x + inset, y + ry, w - 2 * inset, 1, color);
    }
}

/*
 * AnimBlitCover —— 把 XRGB 像素（每像素 u32）**铺满全屏**：
 * 等比缩放到「覆盖」整屏（可能放大或缩小），多余部分居中裁掉（cover 语义，
 * 不拉伸变形）。缩放用 16.16 定点最近邻（内核无 FPU/SSE）。
 */
static void AnimBlitCover(const uint32_t *px, uint32_t sw, uint32_t sh)
{
    uint32_t W = fb_width(), H = fb_height();
    if (!g_fb.ready || !px || sw == 0 || sh == 0 || W == 0 || H == 0)
        return;

    /* 取较大的缩放比，使目标尺寸 >= 屏幕尺寸（铺满），再居中裁剪。 */
    uint32_t dst_w, dst_h;
    if ((uint64_t)W * sh >= (uint64_t)H * sw) {
        dst_w = W;
        dst_h = (uint32_t)((uint64_t)sh * W / sw);
    } else {
        dst_h = H;
        dst_w = (uint32_t)((uint64_t)sw * H / sh);
    }
    if (dst_w < W) dst_w = W;
    if (dst_h < H) dst_h = H;

    uint32_t off_x = (dst_w - W) / 2;
    uint32_t off_y = (dst_h - H) / 2;
    uint32_t sx_fp = (uint32_t)(((uint64_t)sw << 16) / dst_w);
    uint32_t sy_fp = (uint32_t)(((uint64_t)sh << 16) / dst_h);

    for (uint32_t y = 0; y < H; y++) {
        uint32_t sy = (uint32_t)(((uint64_t)(y + off_y) * sy_fp) >> 16);
        if (sy >= sh)
            sy = sh - 1;
        const uint32_t *srow = px + (uint64_t)sy * sw;
        volatile uint32_t *d = (volatile uint32_t *)
            (g_fb.base + (uint64_t)y * g_fb.pitch);
        if (sx_fp == (1u << 16) && off_x + W <= sw) {
            memcpy((void *)d, srow + off_x, (size_t)W * 4);   /* 1:1 快路径 */
        } else {
            for (uint32_t x = 0; x < W; x++) {
                uint32_t sx = (uint32_t)(((uint64_t)(x + off_x) * sx_fp) >> 16);
                if (sx >= sw)
                    sx = sw - 1;
                d[x] = srow[sx];
            }
        }
    }
}

/* ------------------------------------------------------------------ BMP 解码 */

/*
 * AnimDecodeBmp —— 解码 24/32bpp、BI_RGB、单平面 BMP 到 kmalloc 的 XRGB 像素
 * 缓冲（上->下），成功返回缓冲指针（调用方 kfree），失败返回 NULL。
 * 仅支持无压缩真彩色（最常见的启动图/BMP 导出格式）。
 */
static uint32_t *AnimDecodeBmp(const uint8_t *d, uint32_t len,
                               uint32_t *out_w, uint32_t *out_h)
{
    if (len < 54 || d[0] != 'B' || d[1] != 'M')
        return NULL;
    uint32_t data_off = AnimLe32(d + 10);
    uint32_t hdr_size = AnimLe32(d + 14);
    int32_t  w        = (int32_t)AnimLe32(d + 18);
    int32_t  h        = (int32_t)AnimLe32(d + 22);
    uint32_t bpp      = AnimLe16(d + 28);
    uint32_t comp     = AnimLe32(d + 30);
    if (hdr_size < 40 || w <= 0 || h == 0 || comp != 0)
        return NULL;
    if (bpp != 24 && bpp != 32)
        return NULL;

    bool topdown = (h < 0);
    uint32_t aw = (uint32_t)w;
    uint32_t ah = (uint32_t)(topdown ? -h : h);
    if ((uint64_t)aw * ah * 4 > ANIM_CUSTOM_MAX)
        return NULL;          /* 解码后过大：视为配置错误，回退内嵌图 */
    uint64_t row = ((uint64_t)aw * bpp + 31) / 32 * 4;
    if (row > 0xFFFFFFFFull)
        return NULL;
    if ((uint64_t)data_off + row * ah > len)
        return NULL;

    uint32_t *px = (uint32_t *)kmalloc((size_t)((uint64_t)aw * ah * 4));
    if (!px)
        return NULL;

    uint32_t bpp_bytes = bpp / 8;
    for (uint32_t y = 0; y < ah; y++) {
        uint32_t sy = topdown ? y : (ah - 1 - y);
        const uint8_t *s = d + data_off + (uint64_t)sy * row;
        uint32_t *o = px + (uint64_t)y * aw;
        for (uint32_t x = 0; x < aw; x++) {
            uint8_t b = s[x * bpp_bytes + 0];
            uint8_t g = s[x * bpp_bytes + 1];
            uint8_t r = s[x * bpp_bytes + 2];
            o[x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }
    *out_w = aw;
    *out_h = ah;
    return px;
}

/* 内嵌启动图查找：id 为空或未命中时回退到表内第一张（便于新增 bmp 即生效）。 */
static const bootanim_blob_t *AnimFindBlob(const char *id)
{
    const bootanim_blob_t *first = NULL;
    for (const bootanim_blob_t *b = g_bootanim_blobs; b->name; b++) {
        if (!first)
            first = b;
        if (id && id[0] && strcmp(b->name, id) == 0)
            return b;
    }
    return first;
}

/*
 * AnimLoadCustom —— 按 CustomLogo 配置加载自定义启动图（BMP）。
 * DirectPath 形如 "cd0:/boot/anim/x.bmp" / "disk0:/images/x.bmp"：
 *   cd0:/cdrom0:/cd:  -> 内核 ISO9660（仅光盘启动挂载后可用）
 *   disk0:/hdd0:/hd:  -> 内核 VFS（FS_PORT -> Ring3 FAT32），即物理盘 0
 * 无前缀的 Path 走 VFS，失败再试 ISO9660（覆盖单光盘启动场景）。
 */
static uint32_t *AnimLoadCustom(const system_config_t *cfg, uint32_t *out_w,
                                uint32_t *out_h)
{
    const char *path = NULL;
    bool force_iso = false, force_disk = false;
    static const char *pref_cd[]   = { "cd0:", "cdrom0:", "cd:" };
    static const char *pref_disk[] = { "disk0:", "hdd0:", "hd0:" };

    if (cfg->custom_logo_direct_path[0]) {
        path = cfg->custom_logo_direct_path;
        for (size_t i = 0; i < sizeof(pref_cd) / sizeof(pref_cd[0]); i++) {
            size_t n = strlen(pref_cd[i]);
            if (strncmp(path, pref_cd[i], n) == 0) { force_iso = true; path += n; break; }
        }
        if (!force_iso) {
            for (size_t i = 0; i < sizeof(pref_disk) / sizeof(pref_disk[0]); i++) {
                size_t n = strlen(pref_disk[i]);
                if (strncmp(path, pref_disk[i], n) == 0) { force_disk = true; path += n; break; }
            }
        }
    } else if (cfg->custom_logo_path[0]) {
        path = cfg->custom_logo_path;
    } else {
        kprintf("[boot] anim: CustomLogo enabled but no Path/DirectPath\n");
        return NULL;
    }
    while (*path == '/')
        path++;
    if (!*path)
        return NULL;

    uint8_t *raw = (uint8_t *)kmalloc(ANIM_CUSTOM_MAX);
    if (!raw)
        return NULL;
    uint32_t n = 0;
    int rc = -1;
    if (force_iso) {
        rc = IsoIsMounted() ? IsoReadFile(path, raw, ANIM_CUSTOM_MAX, &n) : -1;
    } else if (force_disk) {
        rc = kern_fs_read_file(path, raw, ANIM_CUSTOM_MAX, &n);
    } else {
        rc = kern_fs_read_file(path, raw, ANIM_CUSTOM_MAX, &n);
        if (rc != 0 && IsoIsMounted())
            rc = IsoReadFile(path, raw, ANIM_CUSTOM_MAX, &n);
    }
    if (rc != 0 || n == 0) {
        kprintf("[boot] anim: custom logo '%s' load failed (rc=%d)\n", path, rc);
        kfree(raw);
        return NULL;
    }
    uint32_t *px = AnimDecodeBmp(raw, n, out_w, out_h);
    kfree(raw);
    if (!px)
        kprintf("[boot] anim: custom logo '%s' not a supported BMP "
                "(need 24/32bpp uncompressed)\n", path);
    return px;
}

/* ------------------------------------------------------------------ 进度条 */

static void AnimDrawProgress(void)
{
    /* 轨道：黑底圆角（两侧圆角） */
    AnimRoundRect(g_bar_x, g_bar_y, g_bar_w, g_bar_h, g_bar_r, FB_BLACK);
    if (g_anim_pct == 0)
        return;
    /* 填充：白色圆角；宽度按比例，最窄也保持药丸形 */
    uint32_t fw = (uint32_t)((uint64_t)g_bar_w * g_anim_pct / 100);
    if (fw < g_bar_h)
        fw = g_bar_h;
    if (fw > g_bar_w)
        fw = g_bar_w;
    AnimRoundRect(g_bar_x, g_bar_y, fw, g_bar_h, g_bar_r, FB_WHITE);
}

/* ------------------------------------------------------------------ 对外接口 */

void bootanim_setup(const system_config_t *cfg)
{
    g_anim_ready = false;
    g_anim_pct = 0;
    if (!fb_available()) {
        kprintf("[boot] anim: no framebuffer (text mode), skip boot screen\n");
        return;
    }

    uint32_t W = fb_width(), H = fb_height();
    bool show_logo     = cfg ? (cfg->have_show_logo ? cfg->show_logo : true) : true;
    bool show_progress = cfg ? (cfg->have_show_progress ? cfg->show_progress : true) : true;
    const char *logo_id = (cfg && cfg->have_boot_logo_id) ? cfg->boot_logo_id : "";

    /* 启动画面：整屏清黑（进度条「黑底」即以此为基础） */
    fb_clear(FB_BLACK);

    /* ---- 启动图标 ---- */
    if (show_logo) {
        uint32_t *owned = NULL, iw = 0, ih = 0;
        const uint32_t *px = NULL;
        const char *src = "none";

        if (cfg && cfg->have_custom_logo && cfg->custom_logo_enabled) {
            owned = AnimLoadCustom(cfg, &iw, &ih);
            if (owned && iw && ih) { px = owned; src = "custom"; }
        }
        if (!px) {
            const bootanim_blob_t *b = AnimFindBlob(logo_id);
            if (b && b->end > b->start + 16) {
                const uint8_t *hdr = b->start;
                if (AnimLe32(hdr) == BOOTANIM_MAGIC) {
                    iw = AnimLe32(hdr + 4);
                    ih = AnimLe32(hdr + 8);
                    px = (const uint32_t *)(hdr + 16);
                    src = b->name;
                } else {
                    kprintf("[boot] anim: embedded '%s' bad magic\n", b->name);
                }
            }
        }

        if (px && iw && ih) {
            /* 铺满全屏（cover）：等比放大/缩小至覆盖整屏并居中裁剪 */
            AnimBlitCover(px, iw, ih);
            kprintf("[boot] anim: logo '%s' %ux%u -> fullscreen %ux%u (cover)\n",
                    src, iw, ih, W, H);
        } else {
            kprintf("[boot] anim: no logo image (BootLogoID='%s'); "
                    "put a .bmp in boot/anim/\n", logo_id);
        }
        if (owned)
            kfree(owned);
    } else {
        kprintf("[boot] anim: logo hidden (ShowLogo=false)\n");
    }

    /* ---- 进度条几何：屏幕 3/4 高度处、屏宽 1/3 居中，高 14px（药丸圆角） ---- */
    g_anim_show_progress = show_progress;
    if (show_progress) {
        g_bar_h = (H >= 480) ? 14 : 8;
        g_bar_r = g_bar_h / 2;
        g_bar_w = W / 3;
        if (g_bar_w < 200) g_bar_w = (W > 200) ? 200 : W;
        if (g_bar_w > 640) g_bar_w = 640;
        g_bar_x = (W > g_bar_w) ? (W - g_bar_w) / 2 : 0;
        g_bar_y = H * 3 / 4;
        AnimDrawProgress();
        kprintf("[boot] anim: progress bar %ux%u @ (%u,%u) r=%u\n",
                g_bar_w, g_bar_h, g_bar_x, g_bar_y, g_bar_r);
    } else {
        kprintf("[boot] anim: progress bar hidden (ShowProgress=false)\n");
    }

    g_anim_t0_ns = clock_monotonic_ns();
    g_anim_ready = true;
}

void bootanim_progress(uint32_t pct)
{
    if (!g_anim_ready || !g_anim_show_progress)
        return;
    if (pct > 100)
        pct = 100;
    if (pct <= g_anim_pct)      /* 单调不减：避免里程碑乱序导致回退 */
        return;
    g_anim_pct = pct;
    AnimDrawProgress();
    kprintf("[boot] anim: progress %u%%\n", pct);
}

void bootanim_finish(void)
{
    if (!g_anim_ready)
        return;
    uint64_t start = (g_anim_t0_ns != 0) ? g_anim_t0_ns : clock_monotonic_ns();

    /* 在剩余的最短展示时间内，把进度从当前里程碑平滑补到 100%。 */
    for (;;) {
        uint64_t el_ms = (clock_monotonic_ns() - start) / 1000000ULL;
        if (el_ms >= BOOTANIM_MIN_MS)
            break;
        if (g_anim_show_progress) {
            uint32_t pct = g_anim_pct +
                (uint32_t)((uint64_t)(100 - g_anim_pct) * el_ms / BOOTANIM_MIN_MS);
            if (pct > 99)
                pct = 99;
            if (pct > g_anim_pct) {
                g_anim_pct = pct;
                AnimDrawProgress();
            }
        }
        msleep(10);   /* 让出 CPU（真实睡眠，不忙等） */
    }
    if (g_anim_show_progress && g_anim_pct < 100) {
        g_anim_pct = 100;
        AnimDrawProgress();
    }
    kprintf("[boot] anim: boot screen done (held at least %u ms, progress=%s)\n",
            BOOTANIM_MIN_MS, g_anim_show_progress ? "100%" : "disabled");
}

bool bootanim_boot_done(void)
{
    return g_boot_seq_done;
}

void bootanim_handoff(void)
{
    if (g_boot_seq_done)
        return;
    g_boot_seq_done = true;
    kprintf("[boot] anim: handoff -> display server takes over "
            "(all drivers + services initialized)\n");
}
