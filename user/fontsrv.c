/*
 * user/fontsrv.c
 * -----------------------------------------------------------------------------
 * SukiOS 字体服务（Ring3，FreeType 渲染引擎）。
 *
 * 角色：常驻字体服务，是用户态「字体渲染」的入口程序（由 shell 启动早期
 *       suki_exec 拉起，后台常驻）。持有 FreeType FT_Library 与按路径缓存的
 *       FT_Face；对外经 FONT_PORT 接收渲染请求（见 font_ipc.h）。
 *
 * 渲染路径：
 *   1) 按请求中的 font_path 首次加载字体：从 FAT32 磁盘读全部字节到内存，
 *      FT_New_Memory_Face（FT_OPEN_MEMORY，不依赖宿主 stdio）得到 FT_Face 并缓存。
 *   2) 解析文本（UTF-8 或空格分隔 Unicode 码点），逐字：
 *        FT_Set_Pixel_Sizes -> FT_Load_Char(FT_LOAD_RENDER) 得到 8-bit 灰度位图；
 *        字形缓存（按 face+glyph_index+pixel_size）命中则跳过光栅化；
 *        把位图按文本颜色 + alpha 合成到帧缓冲（本服务同样经 SYS_FRAMEBUFFER_MAP
 *        映射物理帧缓冲，与 display_server 写同一物理内存，叠加显示）。
 *   3) 经请求头中的 msgh_local_port 回执 FONT_MSG_RENDER_DONE（含统计：字形数、
 *      缓存命中、包围盒），供 pchfnt 确认完成。
 *
 * 内存字体缓存（OSDev 推荐）：TTF 文件字节常驻内存，FT_Face 缓存避免重复
 *       解析；单字形位图 LRU 缓存避免长文本重复光栅化。
 *
 * 注意：headless（-display none）下 SYS_FRAMEBUFFER_MAP 返回 enabled=0，本服务
 *       降级为「仅渲染 + serial 报告统计」，不写屏也不 panic。
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include "lib/suki.h"
#include <stdio.h>
#include "font_ipc.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H

/* ---- 帧缓冲映射（与 display_server 共用物理帧缓冲，结构布局须一致）---- */
typedef struct fb_map_result {
    uint32_t enabled;
    uint32_t _pad0;
    uint64_t fb_user_va;
    uint64_t fb_phys;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t cfg_width;
    uint32_t cfg_height;
} fb_map_result_t;

static volatile uint32_t *g_fb = NULL;
static uint32_t g_fb_w = 0, g_fb_h = 0, g_fb_pitch = 0;

/* ---- FreeType 全局 ---- */
static FT_Library g_ftlib = NULL;

/* 字体缓存：路径 -> 内存字节 + FT_Face */
#define FONT_CACHE_MAX 8
struct font_entry {
    char     path[256];
    uint8_t *data;        /* TTF 文件字节（常驻内存） */
    uint32_t size;
    FT_Face  face;
    int      used;
} g_fonts[FONT_CACHE_MAX];

static struct font_entry *font_get(const char *path)
{
    for (int i = 0; i < FONT_CACHE_MAX; i++) {
        if (g_fonts[i].data && strcmp(g_fonts[i].path, path) == 0) {
            g_fonts[i].used++;
            return &g_fonts[i];
        }
    }
    /* 载入：读磁盘全部字节。
     * 兼容：若 FatFs 未启用长文件名（LFN），长文件名打开会失败，此时依次回退
     * 到 8.3 短名候选（RESOUR~1.TTF 等），确保字体仍能加载。 */
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        static const char *short_names[] = {
            "/FONTS/RESOUR~1.TTF",
            "/FONTS/RESOURCE.TTF",
            "/FONTS/FONT.TTF"
        };
        for (int s = 0; s < 3 && fd < 0; s++) {
            fd = open(short_names[s], O_RDONLY);
        }
        if (fd < 0) {
            printf("[fontsrv] font open failed: %s (and 8.3 fallbacks)\n", path);
            return NULL;
        }
    }
    uint8_t *buf = NULL; uint32_t cap = 65536;
    buf = (uint8_t*)malloc(cap);
    if (!buf) { close(fd); return NULL; }
    long n; uint32_t off = 0;
    while ((n = read(fd, buf + off, cap - off)) > 0) {
        off += (uint32_t)n;
        if (off >= cap) {
            cap *= 2;
            uint8_t *nb = (uint8_t*)realloc(buf, cap);
            if (!nb) { free(buf); close(fd); return NULL; }
            buf = nb;
        }
    }
    close(fd);
    if (off == 0) { free(buf); return NULL; }

    FT_Face face = NULL;
    if (FT_New_Memory_Face(g_ftlib, buf, (FT_Long)off, 0, &face) != 0) {
        free(buf); return NULL;
    }
    int slot = -1;
    for (int i = 0; i < FONT_CACHE_MAX; i++) if (!g_fonts[i].data) { slot = i; break; }
    if (slot < 0) {
        int oldest = 0;
        for (int i = 1; i < FONT_CACHE_MAX; i++)
            if (g_fonts[i].used < g_fonts[oldest].used) oldest = i;
        slot = oldest;
        if (g_fonts[slot].data) { FT_Done_Face(g_fonts[slot].face); free(g_fonts[slot].data); }
    }
    struct font_entry *e = &g_fonts[slot];
    strncpy(e->path, path, 255); e->path[255] = 0;
    e->data = buf; e->size = off; e->face = face; e->used = 1;
    printf("[fontsrv] loaded font %s (%u bytes, %ld faces)\n", path, off, face->num_faces);
    return e;
}

/* ---- 字形位图缓存（应用层 LRU）---- */
#define GLYPH_CACHE_MAX 2048
struct glyph_cache {
    FT_Face   face;
    uint32_t  glyph;
    uint32_t  size;
    int32_t   w, h;
    int32_t   top, left;
    int32_t   advance;    /* 26.6 */
    uint8_t  *bits;       /* 8-bit 灰度，w*h 字节 */
    int       used;
} g_glyph[GLYPH_CACHE_MAX];
static int g_glyph_count = 0;

static uint32_t glyph_hash(FT_Face face, uint32_t glyph, uint32_t size)
{
    uint64_t v = (uint64_t)(uintptr_t)face ^ ((uint64_t)glyph << 3) ^ ((uint64_t)size << 17);
    return (uint32_t)(v ^ (v >> 32)) % GLYPH_CACHE_MAX;
}

/* 返回缓存槽（命中或新建），miss 时 bits=NULL 需调用方光栅化后 fill */
static struct glyph_cache *glyph_lookup(FT_Face face, uint32_t glyph, uint32_t size, int *hit)
{
    uint32_t h = glyph_hash(face, glyph, size);
    for (int i = 0; i < GLYPH_CACHE_MAX; i++) {
        int idx = (h + i) % GLYPH_CACHE_MAX;
        struct glyph_cache *g = &g_glyph[idx];
        if (g->bits && g->face == face && g->glyph == glyph && g->size == size) {
            g->used++;
            *hit = 1;
            return g;
        }
    }
    /* 找空槽或 LRU 淘汰 */
    int victim = -1;
    for (int i = 0; i < GLYPH_CACHE_MAX; i++) {
        int idx = (h + i) % GLYPH_CACHE_MAX;
        if (!g_glyph[idx].bits) { victim = idx; break; }
    }
    if (victim < 0) {
        int oldest = 0;
        for (int i = 1; i < GLYPH_CACHE_MAX; i++)
            if (g_glyph[i].used < g_glyph[oldest].used) oldest = i;
        victim = oldest;
        free(g_glyph[victim].bits);
        g_glyph_count--;
    }
    struct glyph_cache *g = &g_glyph[victim];
    g->face = face; g->glyph = glyph; g->size = size;
    g->bits = NULL; g->w = g->h = 0; g->used = 1;
    *hit = 0;
    return g;
}

/* ---- 像素合成：8-bit 灰度 alpha 混合到 xRGB32 帧缓冲 ---- */
static void blit_glyph(const uint8_t *bits, int w, int h, int gx, int gy, uint32_t color)
{
    if (!g_fb) return;
    uint8_t cr = (color >> 16) & 0xFF, cg = (color >> 8) & 0xFF, cb = color & 0xFF;
    for (int j = 0; j < h; j++) {
        int py = gy + j;
        if (py < 0 || (uint32_t)py >= g_fb_h) continue;
        for (int i = 0; i < w; i++) {
            int px = gx + i;
            if (px < 0 || (uint32_t)px >= g_fb_w) continue;
            uint8_t a = bits[j * w + i];
            if (a == 0) continue;
            volatile uint32_t *dst = &g_fb[py * (g_fb_pitch / 4) + px];
            uint32_t d = *dst;
            uint8_t dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
            uint32_t ia = 255 - a;
            uint8_t r = (uint8_t)((cr * a + dr * ia) / 255);
            uint8_t g = (uint8_t)((cg * a + dg * ia) / 255);
            uint8_t b = (uint8_t)((cb * a + db * ia) / 255);
            *dst = (0xFF << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

/* ---- UTF-8 解码：返回码点，*p 前进 ---- */
static uint32_t utf8_next(const char **p)
{
    const unsigned char *s = (const unsigned char*)*p;
    uint32_t cp;
    if (*s < 0x80) { cp = *s; (*p)++; }
    else if ((*s & 0xE0) == 0xC0) { cp = ((*s & 0x1F) << 6) | (s[1] & 0x3F); *p += 2; }
    else if ((*s & 0xF0) == 0xE0) { cp = ((*s & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); *p += 3; }
    else if ((*s & 0xF8) == 0xF0) { cp = ((*s & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F); *p += 4; }
    else { cp = '?'; (*p)++; }
    return cp;
}

/* 解析文本为码点序列（unicode_mode=1 时按空格分隔十进制/0x 十六进制） */
static int parse_codepoints(const char *text, int unicode_mode, uint32_t *out, int maxn)
{
    int n = 0;
    if (unicode_mode) {
        const char *p = text;
        while (*p && n < maxn) {
            while (*p == ' ') p++;
            if (!*p) break;
            char *end;
            uint32_t cp = (uint32_t)strtoul(p, &end, 0);
            if (end == p) break;
            out[n++] = cp;
            p = end;
        }
    } else {
        const char *p = text;
        while (*p && n < maxn) {
            uint32_t cp = utf8_next(&p);
            if (cp == 0) break;
            out[n++] = cp;
        }
    }
    return n;
}

/* ---- 处理一次渲染请求 ---- */
static void handle_render(font_render_req_t *req, font_render_done_t *done)
{
    done->req_id = req->req_id;
    done->status = 0; done->glyphs = 0; done->cache_hits = 0;
    done->bbox_w = 0; done->bbox_h = 0;

    struct font_entry *fe = font_get(req->font_path);
    if (!fe) { done->status = -1; printf("[fontsrv] font load failed: %s\n", req->font_path); return; }

    FT_Face face = fe->face;
    FT_Set_Pixel_Sizes(face, 0, req->pixel_size);

    uint32_t cps[1024];
    int n = parse_codepoints(req->text, req->unicode_mode, cps, 1024);
    int pen_x = req->x;
    int pen_y = req->y + (int)req->pixel_size;   /* 基线 = 顶部 + 字号 */
    int min_x = pen_x, max_x = pen_x;

    for (int i = 0; i < n; i++) {
        FT_UInt gi = FT_Get_Char_Index(face, cps[i]);
        int hit = 0;
        struct glyph_cache *gc = glyph_lookup(face, gi, req->pixel_size, &hit);
        if (!gc->bits) {
            FT_Load_Glyph(face, gi, FT_LOAD_RENDER);
            FT_GlyphSlot slot = face->glyph;
            int w = slot->bitmap.width, h = slot->bitmap.rows;
            gc->bits = (uint8_t*)malloc((size_t)w * h);
            if (gc->bits) {
                memcpy(gc->bits, slot->bitmap.buffer, (size_t)w * h);
                gc->w = w; gc->h = h;
                gc->left = slot->bitmap_left; gc->top = slot->bitmap_top;
                gc->advance = slot->advance.x;
                g_glyph_count++;
            }
        } else {
            done->cache_hits++;
        }
        if (gc->bits) {
            int gx = pen_x + gc->left;
            int gy = pen_y - gc->top;
            blit_glyph(gc->bits, gc->w, gc->h, gx, gy, req->color);
            if (gx < min_x) min_x = gx;
            int right = gx + gc->w;
            if (right > max_x) max_x = right;
            if ((gy - gc->top) < 0) {}
            int bottom = gy + gc->h;
            if (bottom > (int)done->bbox_h) done->bbox_h = bottom;
        }
        pen_x += gc->advance >> 6;
        done->glyphs++;
    }
    done->bbox_w = max_x - min_x;
    if ((uint32_t)done->bbox_h < (uint32_t)req->pixel_size) done->bbox_h = (int32_t)req->pixel_size;
}

/* ---- 主循环 ---- */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (FT_Init_FreeType(&g_ftlib) != 0) {
        printf("[fontsrv] FT_Init_FreeType failed\n");
        return 1;
    }

    fb_map_result_t res;
    memset(&res, 0, sizeof res);
    if (suki_syscall2(SYS_FRAMEBUFFER_MAP, (uint64_t)&res, 0) == 0 && res.enabled) {
        g_fb = (volatile uint32_t*)(uintptr_t)res.fb_user_va;
        g_fb_w = res.width; g_fb_h = res.height; g_fb_pitch = res.pitch;
        printf("[fontsrv] framebuffer mapped %ux%u pitch=%u\n", g_fb_w, g_fb_h, g_fb_pitch);
    } else {
        printf("[fontsrv] framebuffer NOT available (headless?), rendering-only mode\n");
    }

    sys_port_claim(FONT_PORT);
    printf("[fontsrv] font service online (FreeType, FONT_PORT=%d)\n", FONT_PORT);

    static uint8_t rxbuf[sizeof(mach_msg_header_t) + sizeof(font_render_req_t) + 64];
    for (;;) {
        uint64_t r = mach_msg_recv(rxbuf, sizeof rxbuf, FONT_PORT);
        if (r != 0) { sys_yield(); continue; }
        mach_msg_header_t *h = (mach_msg_header_t*)rxbuf;
        if (h->msgh_id != FONT_MSG_RENDER) { sys_yield(); continue; }
        font_render_req_t *req = (font_render_req_t*)rxbuf;
        font_render_done_t done;
        memset(&done, 0, sizeof done);
        handle_render(req, &done);

        printf("[fontsrv] rendered req=%u glyphs=%d hits=%d bbox=%dx%d status=%d\n",
               done.req_id, done.glyphs, done.cache_hits, done.bbox_w, done.bbox_h, done.status);

        /* 回执到请求方 local_port */
        done.h.msgh_bits = 0;
        done.h.msgh_size = sizeof(done);
        done.h.msgh_remote_port = h->msgh_local_port;
        done.h.msgh_local_port = FONT_PORT;
        done.h.msgh_id = FONT_MSG_RENDER_DONE;
        done.h.msgh_reserved = 0;
        mach_msg_send(&done, sizeof(done));

        sys_yield();
    }
    return 0;
}
