/*
 * user/fontsrv.c
 * -----------------------------------------------------------------------------
 * SukiOS 字体服务（Ring3，FreeType 渲染引擎）。
 *
 * 角色：常驻字体服务，是用户态「字体光栅化」的唯一入口（由 shell 启动早期拉起，
 *       后台常驻）。持有 FT_Library 与按路径缓存的 FT_Face；对外经 FONT_PORT 接收
 *       渲染/测量请求（见 font_ipc.h）。
 *
 * v2 设计（离屏渲染，不再自带合成器）：
 *   - 客户端（libsui / pchfnt）经 FONT_MSG_REGISTER 把自身离屏画布（OOL 物理页）
 *     注册给本服务；本服务把该物理页映射进自身地址空间并持久持有，返回 handle。
 *   - 客户端绘制文本时发 FONT_MSG_RENDER_BUF(handle, x, y, size, color, clip,
 *     font, text)；本服务用 FreeType 把字形光栅化为 8-bit 灰度位图，按 alpha 混合进
 *     已注册的离屏缓冲（尊重裁剪矩形），回执 FONT_MSG_RENDER_DONE（含统计）。
 *   - 宽度测量走 FONT_MSG_MEASURE；窗口销毁走 FONT_MSG_UNREGISTER 解映射。
 *
 * 本服务【绝不】写入物理屏幕帧缓冲——文本合成完全发生在客户端缓冲里，由显示服务
 * 后续统一合成上屏。这与「显示服务是唯一合成器」的架构约定一致。
 *
 * 内存字体缓存（OSDev 推荐）：TTF 文件字节常驻内存，FT_Face 缓存避免重复解析；
 * 单字形位图 LRU 缓存避免长文本重复光栅化。
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
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
    /* 载入：读磁盘全部字节。长文件名打开失败时回退到 8.3 短名候选。 */
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        static const char *short_names[] = {
            "/FONTS/RESOUR~1.TTF",
            "/FONTS/RESOURCE.TTF",
            "/FONTS/FONT.TTF"
        };
        for (int s = 0; s < 3 && fd < 0; s++)
            fd = open(short_names[s], O_RDONLY);
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

static struct glyph_cache *glyph_lookup(FT_Face face, uint32_t glyph, uint32_t size, int *hit)
{
    uint32_t h = glyph_hash(face, glyph, size);
    for (int i = 0; i < GLYPH_CACHE_MAX; i++) {
        int idx = (h + i) % GLYPH_CACHE_MAX;
        struct glyph_cache *g = &g_glyph[idx];
        if (g->bits && g->face == face && g->glyph == glyph && g->size == size) {
            g->used++; *hit = 1; return g;
        }
    }
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

/* ---- 已注册离屏缓冲表（handle -> 映射 VA + 尺寸）---- */
#define FONT_BUF_MAX 32
typedef struct {
    uint32_t handle;
    uint64_t va;          /* fontsrv 内映射 VA（写目标） */
    uint32_t w, h;        /* 像素宽高 */
    uint32_t size;        /* 字节数 */
    bool     used;
} font_buf_t;
static font_buf_t g_bufs[FONT_BUF_MAX];
static uint32_t  g_handle_next = 1;

static font_buf_t *buf_by_handle(uint32_t h)
{
    for (int i = 0; i < FONT_BUF_MAX; i++)
        if (g_bufs[i].used && g_bufs[i].handle == h) return &g_bufs[i];
    return NULL;
}

/* ---- 像素合成：8-bit 灰度 alpha 混合到 xRGB32 离屏缓冲（尊重裁剪）---- */
static void blit_glyph_buf(uint32_t *base, uint32_t buf_w, uint32_t buf_h,
                           const uint8_t *bits, int w, int h, int gx, int gy,
                           uint32_t color, int cx0, int cy0, int cx1, int cy1)
{
    uint8_t cr = (color >> 16) & 0xFF, cg = (color >> 8) & 0xFF, cb = color & 0xFF;
    for (int j = 0; j < h; j++) {
        int py = gy + j;
        if (py < 0 || (uint32_t)py >= buf_h) continue;
        if (py < cy0 || py >= cy1) continue;
        for (int i = 0; i < w; i++) {
            int px = gx + i;
            if (px < 0 || (uint32_t)px >= buf_w) continue;
            if (px < cx0 || px >= cx1) continue;
            uint8_t a = bits[j * w + i];
            if (a == 0) continue;
            uint32_t d = base[(uint64_t)py * buf_w + px];
            uint8_t dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
            uint32_t ia = 255 - a;
            uint8_t r = (uint8_t)((cr * a + dr * ia) / 255);
            uint8_t g = (uint8_t)((cg * a + dg * ia) / 255);
            uint8_t b = (uint8_t)((cb * a + db * ia) / 255);
            base[(uint64_t)py * buf_w + px] = (0xFFu << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

/* ---- 处理一次「渲染到离屏缓冲」请求 ---- */
static void handle_render_buf(const font_render_buf_req_t *req, font_render_done_t *done)
{
    done->status = 0; done->glyphs = 0; done->cache_hits = 0;
    done->bbox_w = 0; done->bbox_h = 0;

    font_buf_t *b = buf_by_handle(req->handle);
    if (!b) { done->status = -2; printf("[fontsrv] unknown buffer handle %u\n", req->handle); return; }

    struct font_entry *fe = font_get(req->font_path);
    if (!fe) { done->status = -1; printf("[fontsrv] font load failed: %s\n", req->font_path); return; }

    FT_Face face = fe->face;
    FT_Set_Pixel_Sizes(face, 0, req->pixel_size);

    int cx0 = req->clip_x, cy0 = req->clip_y;
    int cx1 = req->clip_x + req->clip_w, cy1 = req->clip_y + req->clip_h;

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
            blit_glyph_buf((uint32_t*)(uintptr_t)b->va, b->w, b->h,
                           gc->bits, gc->w, gc->h, gx, gy, req->color,
                           cx0, cy0, cx1, cy1);
            if (gx < min_x) min_x = gx;
            int right = gx + gc->w;
            if (right > max_x) max_x = right;
            int bottom = gy + gc->h;
            if (bottom > (int)done->bbox_h) done->bbox_h = bottom;
        }
        pen_x += gc->advance >> 6;
        done->glyphs++;
    }
    done->bbox_w = max_x - min_x;
    if ((uint32_t)done->bbox_h < (uint32_t)req->pixel_size) done->bbox_h = (int32_t)req->pixel_size;
}

/* ---- 处理一次「测量宽度」请求（不渲染，仅返回 advance 像素宽）---- */
static int32_t handle_measure(const font_measure_req_t *req)
{
    struct font_entry *fe = font_get(req->font_path);
    if (!fe) { printf("[fontsrv] measure: font load failed: %s\n", req->font_path); return -1; }
    FT_Face face = fe->face;
    FT_Set_Pixel_Sizes(face, 0, req->pixel_size);
    uint32_t cps[1024];
    int n = parse_codepoints(req->text, req->unicode_mode, cps, 1024);
    int pen = 0;
    for (int i = 0; i < n; i++) {
        FT_UInt gi = FT_Get_Char_Index(face, cps[i]);
        if (FT_Load_Glyph(face, gi, FT_LOAD_DEFAULT) == 0)
            pen += (int)(face->glyph->advance.x >> 6);
        else
            pen += (int)req->pixel_size / 2;
    }
    return (int32_t)pen;
}

/* ---- 主循环 ---- */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (FT_Init_FreeType(&g_ftlib) != 0) {
        printf("[fontsrv] FT_Init_FreeType failed\n");
        return 1;
    }

    sys_port_claim(FONT_PORT);
    printf("[fontsrv] font service online (offscreen render only, FONT_PORT=%d)\n", FONT_PORT);

    static uint8_t rxbuf[sizeof(mach_msg_header_t) + sizeof(font_render_buf_req_t) + 64];
    for (;;) {
        uint64_t r = mach_msg_recv(rxbuf, sizeof rxbuf, FONT_PORT);
        if (r != 0) { sys_yield(); continue; }
        mach_msg_header_t *h = (mach_msg_header_t*)rxbuf;

        switch (h->msgh_id) {
        case FONT_MSG_REGISTER: {
            font_register_req_t *req = (font_register_req_t*)rxbuf;
            /* req->ool.address 已是本服务内映射的 VA；持久持有，不在此解映射 */
            uint32_t handle = 0;
            int slot = -1;
            for (int i = 0; i < FONT_BUF_MAX; i++) if (!g_bufs[i].used) { slot = i; break; }
            if (slot < 0) {
                int oldest = 0;
                for (int i = 1; i < FONT_BUF_MAX; i++)
                    if (g_bufs[i].handle < g_bufs[oldest].handle) oldest = i;
                slot = oldest;
                if (g_bufs[slot].used) mach_msg_destroy(g_bufs[slot].va);
            }
            if (slot >= 0) {
                font_buf_t *b = &g_bufs[slot];
                b->used = true;
                b->handle = g_handle_next++;
                b->va = req->ool.address;
                b->w = req->buf_w; b->h = req->buf_h;
                b->size = (uint32_t)(req->buf_w * req->buf_h * 4);
                handle = b->handle;
                printf("[fontsrv] buffer registered handle=%u (%ux%u)\n", handle, b->w, b->h);
            }
            font_register_done_t d; memset(&d, 0, sizeof d);
            d.h.msgh_bits = MACH_SEND_MSG;
            d.h.msgh_size = sizeof(d);
            d.h.msgh_remote_port = h->msgh_local_port;
            d.h.msgh_local_port  = FONT_PORT;
            d.h.msgh_id = FONT_MSG_REGISTER_DONE;
            d.status = (handle ? 0 : -1);
            d.handle = handle;
            mach_msg_send(&d, sizeof(d));
            break;
        }
        case FONT_MSG_RENDER_BUF: {
            font_render_buf_req_t *req = (font_render_buf_req_t*)rxbuf;
            font_render_done_t done; memset(&done, 0, sizeof done);
            handle_render_buf(req, &done);
            done.h.msgh_bits = MACH_SEND_MSG;
            done.h.msgh_size = sizeof(done);
            done.h.msgh_remote_port = h->msgh_local_port;
            done.h.msgh_local_port  = FONT_PORT;
            done.h.msgh_id = FONT_MSG_RENDER_DONE;
            mach_msg_send(&done, sizeof(done));
            printf("[fontsrv] render handle=%u glyphs=%d hits=%d bbox=%dx%d status=%d\n",
                   req->handle, done.glyphs, done.cache_hits, done.bbox_w, done.bbox_h, done.status);
            break;
        }
        case FONT_MSG_MEASURE: {
            font_measure_req_t *req = (font_measure_req_t*)rxbuf;
            int32_t w = handle_measure(req);
            font_measure_done_t d; memset(&d, 0, sizeof d);
            d.h.msgh_bits = MACH_SEND_MSG;
            d.h.msgh_size = sizeof(d);
            d.h.msgh_remote_port = h->msgh_local_port;
            d.h.msgh_local_port  = FONT_PORT;
            d.h.msgh_id = FONT_MSG_MEASURE_DONE;
            d.status = (w < 0 ? 1 : 0);
            d.width = w;
            mach_msg_send(&d, sizeof(d));
            break;
        }
        case FONT_MSG_UNREGISTER: {
            font_unregister_req_t *req = (font_unregister_req_t*)rxbuf;
            font_buf_t *b = buf_by_handle(req->handle);
            if (b) {
                mach_msg_destroy(b->va);   /* 解映射客户端物理页 */
                printf("[fontsrv] buffer unregistered handle=%u\n", b->handle);
                b->used = false; b->handle = 0; b->va = 0;
            }
            break;
        }
        default:
            sys_yield();
            break;
        }
    }
    return 0;
}
