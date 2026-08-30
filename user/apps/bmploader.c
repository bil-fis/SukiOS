/*
 * user/apps/bmploader.c
 * -----------------------------------------------------------------------------
 * 独立用户态程序（不编入内核，放入 FAT32 ::BIN/BMPLOADER.SKA），由 Shell 经
 * `exec BIN/bmploader IMAGES/SUKIOS.BMP` 之类调用。
 *
 * 用途（用户需求）：在显示服务已进入图形界面后，用本加载器把 BMP 图片显示到
 * 屏幕，以便人工检查当前画面显示是否存在问题——乱码、错位、色彩反序、行对齐
 * 错误等。它从 FAT32 读取 BMP（经 FS_SERVER IPC），解析 BITMAPFILEHEADER +
 * BITMAPINFOHEADER（支持 24bpp / 32bpp），把像素转成 xRGB32，再经新 syscall
 * SYS_DISPLAY_BLIT 让内核把像素写入帧缓冲（显示服务独占的 1280x720@32bpp LFB）。
 *
 * 图像居中绘制：目标 (x,y) = ((1280-w)/2, (720-h)/2)。
 */
#include "lib/suki.h"
#include <ipc/fs_proto.h>

#define SCREEN_W 1280
#define SCREEN_H 720

/* 用户态缺省端口：独立程序用它接收 FS 应答。 */
#define MY_PORT 8

/* 把路径小写转大写（FAT32 短名全大写）。 */
static void upcase_path(char *s)
{
    for (; *s; s++) {
        if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 'a' + 'A');
    }
}

/* 经 FS_SERVER 读取整个文件到 out（out 由调用方分配，cap 为容量）。
 * 返回实际读取字节数，失败返回 0。
 *
 * 消息布局必须严格为：mach_msg_header_t(24B) + fs_read_at_req_t(8B) + NUL 结尾文件名。
 * FS_SERVER 端以 payload = reqbuf + sizeof(mach_msg_header_t) 解析，故请求结构体的
 * 第一个成员必须是 mach_msg_header_t（与 playaudio.c 一致）。早期版本把
 * fs_read_at_req_t 直接放在缓冲开头，导致 FS 端 fname 偏移错位读到空串
 * （f_open("0:") -> FR_INVALID_NAME），是 BMPLOADER 一直 FS error 的根因。 */
typedef struct {
    mach_msg_header_t  h;
    fs_read_at_req_t   r;
    char               name[FS_PATH_MAX];
} fs_read_at_msg_t;

static uint64_t fs_read_whole(const char *path, uint8_t *out, uint64_t cap)
{
    uint64_t pathlen = 0;
    while (path[pathlen]) pathlen++;

    static fs_read_at_msg_t req;   /* 静态缓冲，避免大栈；结构体内存布局正确 */
    for (uint64_t i = 0; i < pathlen; i++) req.name[i] = path[i];
    req.name[pathlen] = '\0';      /* 文件名必须 NUL 结尾（FS 协议要求） */

    uint64_t got = 0;
    uint64_t off = 0;
    for (;;) {
        req.r.offset = (uint32_t)off;
        req.r.length = FS_DATA_MAX;

        /* 发送 READ_AT 请求到 FS_PORT（目标端口填在 msgh_remote_port） */
        req.h.msgh_bits        = 0;
        req.h.msgh_size        = (uint32_t)(sizeof(mach_msg_header_t) + sizeof(fs_read_at_req_t) + (uint32_t)pathlen + 1);
        req.h.msgh_remote_port = FS_PORT;
        req.h.msgh_local_port  = MY_PORT;
        req.h.msgh_id          = FS_MSG_READ_AT;
        req.h.msgh_reserved    = 0;
        if (mach_msg_send(&req, req.h.msgh_size) != 0) {
            u_print("bmploader: FS send failed\n");
            return 0;
        }

        /* 接收应答 */
        uint8_t resp[sizeof(mach_msg_header_t) + sizeof(fs_resp_t) + FS_DATA_MAX];
        if (mach_msg_recv(resp, sizeof(resp), MY_PORT) != 0) {
            u_print("bmploader: FS recv failed\n");
            return 0;
        }
        fs_resp_t *fr = (fs_resp_t *)(resp + sizeof(mach_msg_header_t));
        if (fr->status != 0) {
            u_print("bmploader: FS error status=");
            { char d[16]; u_print(u_utoa_s((uint64_t)fr->status, d, sizeof(d))); }
            u_print("\n");
            return 0;
        }
        uint8_t *data = resp + sizeof(mach_msg_header_t) + sizeof(fs_resp_t);
        if (fr->length == 0) break;                 /* EOF */
        if (got + fr->length > cap) {               /* 超出容量 */
            u_print("bmploader: file too large for buffer\n");
            return 0;
        }
        for (uint32_t i = 0; i < fr->length; i++) out[got + i] = data[i];
        got += fr->length;
        off += fr->length;
        if (fr->length < FS_DATA_MAX) break;        /* 最后一包 */
    }
    return got;
}

/* BMP 解析与显示。返回 0 成功。 */
static int show_bmp(const uint8_t *buf, uint64_t size)
{
    if (size < 54) { u_print("bmploader: file too small\n"); return -1; }
    if (buf[0] != 'B' || buf[1] != 'M') { u_print("bmploader: not a BMP\n"); return -1; }

    /* BITMAPFILEHEADER: data offset at 0x0A (4B) */
    uint32_t data_off = *(const uint32_t *)(buf + 10);
    /* BITMAPINFOHEADER: width @0x12, height @0x16, bpp @0x1C, compression @0x1E */
    int32_t w = *(const int32_t *)(buf + 18);
    int32_t h = *(const int32_t *)(buf + 22);
    uint16_t bpp = *(const uint16_t *)(buf + 28);
    uint32_t compression = *(const uint32_t *)(buf + 30);

    u_print("bmploader: BMP w=");
    { char d[16]; u_print(u_utoa_s(w, d, sizeof(d))); }
    u_print(" h=");
    { char d[16]; u_print(u_utoa_s(h, d, sizeof(d))); }
    u_print(" bpp=");
    { char d[16]; u_print(u_utoa_s(bpp, d, sizeof(d))); }
    u_print("\n");

    if (w <= 0 || h <= 0 || (bpp != 24 && bpp != 32) || compression != 0) {
        u_print("bmploader: unsupported BMP format\n");
        return -1;
    }
    if ((uint64_t)data_off + (uint64_t)w * h * (bpp / 8) > size) {
        u_print("bmploader: truncated pixel data\n");
        return -1;
    }

    uint32_t bw = (uint32_t)w;
    uint32_t bh = (uint32_t)h;
    /* 每行字节数（4 字节对齐） */
    uint32_t row_bytes = ((bw * (bpp / 8) + 3) / 4) * 4;

    /* 分配像素缓冲（xRGB32, 每像素 4B） */
    uint32_t *pixels = (uint32_t *)sys_mmap((uint64_t)bw * bh * 4, SUKI_PROT_WRITE);
    if (!pixels) { u_print("bmploader: oom\n"); return -1; }

    const uint8_t *px = buf + data_off;
    for (uint32_t row = 0; row < bh; row++) {
        /* BMP 自下而上：文件第 row 行对应图像第 (bh-1-row) 行 */
        const uint8_t *src = px + row * row_bytes;
        uint32_t *dst = pixels + (bh - 1 - row) * bw;
        for (uint32_t col = 0; col < bw; col++) {
            uint8_t b, g, r;
            if (bpp == 24) {
                b = src[col * 3 + 0];
                g = src[col * 3 + 1];
                r = src[col * 3 + 2];
            } else { /* 32bpp: BGRA */
                b = src[col * 4 + 0];
                g = src[col * 4 + 1];
                r = src[col * 4 + 2];
            }
            /* xRGB32: (r<<16)|(g<<8)|b —— 与帧缓冲 [B][G][R][X] 内存布局一致 */
            dst[col] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }

    int32_t dx = (int32_t)((SCREEN_W - bw) / 2);
    int32_t dy = (int32_t)((SCREEN_H - bh) / 2);
    uint64_t rc = suki_syscall5(SYS_DISPLAY_BLIT,
                                (uint64_t)pixels, bw, bh, (uint64_t)dx, (uint64_t)dy);
    if (rc != 0) {
        u_print("bmploader: blit failed\n");
        sys_munmap(pixels, (uint64_t)bw * bh * 4);
        return -1;
    }
    u_print("bmploader: displayed BMP at (");
    { char d[16]; u_print(u_utoa_s(dx, d, sizeof(d))); }
    u_print(",");
    { char d[16]; u_print(u_utoa_s(dy, d, sizeof(d))); }
    u_print(")\n");
    sys_munmap(pixels, (uint64_t)bw * bh * 4);
    return 0;
}

int main(int argc, char **argv)
{
    u_print("=== bmploader (BMP display diagnostic) ===\n");
    /* sys_port_claim 返回 0 表示成功（与内核 port_claim 语义一致，
     * 失败返回 (uint64_t)-1）。注意不可写成 !rc —— 那会把成功(0)误判为失败。 */
    if (sys_port_claim(MY_PORT) != 0) {
        u_print("bmploader: port claim failed\n");
        sys_exit(1);
    }
    if (argc < 2) {
        u_print("usage: BMPA <IMAGE.BMP>  (e.g. IMAGES/SUKIOS.BMP)\n");
        sys_exit(1);
    }

    /* 复制并规整路径（FAT32 短名大写） */
    char path[FS_PATH_MAX];
    uint64_t i = 0;
    for (; argv[1][i] && i < FS_PATH_MAX - 1; i++) path[i] = argv[1][i];
    path[i] = '\0';
    upcase_path(path);
    u_print("bmploader: reading ");
    u_print(path);
    u_print("\n");

    /* 分配文件缓冲（BMP 通常较小；上限 8MiB） */
    uint64_t cap = 8u * 1024u * 1024u;
    uint8_t *file = (uint8_t *)sys_mmap(cap, SUKI_PROT_WRITE);
    if (!file) { u_print("bmploader: oom(file)\n"); sys_exit(1); }

    uint64_t n = fs_read_whole(path, file, cap);
    if (n == 0) {
        u_print("bmploader: cannot read file (fs/server not ready? path wrong?)\n");
        sys_munmap(file, cap);
        sys_exit(1);
    }
    u_print("bmploader: read ");
    { char d[16]; u_print(u_utoa_s(n, d, sizeof(d))); }
    u_print(" bytes\n");

    show_bmp(file, n);
    sys_munmap(file, cap);
    sys_exit(0);
    return 0;
}
