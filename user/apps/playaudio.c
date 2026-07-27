/*
 * user/apps/playaudio.c
 * -----------------------------------------------------------------------------
 * SukiOS 独立音频播放器（standalone app）：用 minimp3 解码 MP3 并经
 * SYS_AUDIO_* 系统调用把 PCM 送入内核 Intel HDA 驱动播放。
 *
 * 数据通路：
 *   FS_SERVER(FAT32) --FS_MSG_READ_AT(分块)--> playaudio
 *      --> minimp3 解码为 S16LE PCM
 *      --> sys_audio_write() --> 内核 HDA 播放环(BDL DMA) --> 声卡
 *
 * 关键设计：
 *   1. 流式读取：MP3 文件可达数 MB，绝不整文件载入。以 32KiB 输入环滑动
 *      窗口方式，每次经 FS_MSG_READ_AT 读 <=FS_DATA_MAX(3500B) 补满，
 *      解码消费后 memmove 前移。
 *   2. 背压：sys_audio_write 返回 0（内核环满）时 sys_yield() 让出 CPU，
 *      待 DMA 排空后重试，实现与 100Hz 抢占调度协作的软实时播放。
 *   3. 首帧决定格式：MP3 帧头带采样率/声道，解出首个有效帧后再
 *      sys_audio_open(hz, ch, 16)。
 *
 * 编译要点（见 Makefile APP_CFLAGS）：
 *   - 必须启用 SSE2（x86_64 浮点走 XMM；内核 switch.S 已 fxsave/fxrstor
 *     保存 Ring3 FPU/SSE 上下文，CR4.OSFXSR 已开，用户态用浮点安全）。
 *   - MINIMP3_ONLY_MP3 去掉 Layer1/2 代码，MINIMP3_NO_SIMD 走通用浮点
 *     路径（避免 intrinsics 头依赖），两者显著缩小 ELF 体积以适配
 *     execve 单条 OOL 16 页(64KiB) 加载上限。
 *
 * 用法：exec BIN/playaudio [MP3文件名]     （缺省 MOONHALO.MP3）
 */
#include "lib/suki.h"
#include <ipc/fs_proto.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3        /* 仅 MPEG Layer III，去掉 L1/L2，缩小体积 */
#define MINIMP3_NO_SIMD         /* 通用标量浮点路径（仍需 -msse2 做浮点码生成） */
#include "minimp3.h"

/* ---- FS 分块读客户端（应答收到 APP_PORT） ---- */
static uint8_t g_fsrx[sizeof(mach_msg_header_t) + sizeof(fs_resp_t)
                      + FS_DATA_MAX + 16];

/* CRC32（IEEE，与主机参考实现一致），用于 bench 模式校验解码 PCM：
 * 全曲期望值 699861505（主机 minimp3 同参数解码 moonhalo.mp3 所得） */
static uint32_t g_crc = 0;
static uint32_t crc32(const uint8_t *d, uint32_t n, uint32_t crc)
{
    for (uint32_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
        }
    }
    return crc;
}

/* 从 FS 读取 name 文件 [off, off+len) 字节到 out（len 会截断到 FS_DATA_MAX）。
 * 返回实际字节数；0 = EOF 或错误。 */
static uint32_t fs_read_at(const char *name, uint32_t off, uint32_t len,
                           void *out)
{
    if (len > FS_DATA_MAX) {
        len = FS_DATA_MAX;
    }
    struct {
        mach_msg_header_t h;
        fs_read_at_req_t  r;
        char              name[64];
    } req;
    u_memset(&req, 0, sizeof(req));

    uint32_t nl = 0;
    while (name[nl] && nl < sizeof(req.name) - 1) {
        req.name[nl] = name[nl];
        nl++;
    }
    req.name[nl] = '\0';

    req.h.msgh_bits = 0;
    req.h.msgh_size = (uint32_t)(sizeof(mach_msg_header_t)
                                 + sizeof(fs_read_at_req_t) + nl + 1);
    req.h.msgh_remote_port = FS_PORT;
    req.h.msgh_local_port = APP_PORT;      /* 应答回本 app 端口 */
    req.h.msgh_id = FS_MSG_READ_AT;
    req.h.msgh_reserved = 0;
    req.r.offset = off;
    req.r.length = len;

    if (mach_msg_send(&req, req.h.msgh_size) != MACH_MSG_SUCCESS) {
        return 0;
    }
    if (mach_msg_recv(g_fsrx, sizeof(g_fsrx), APP_PORT) != MACH_MSG_SUCCESS) {
        return 0;
    }
    mach_msg_header_t *h = (mach_msg_header_t *)g_fsrx;
    if (h->msgh_id != FS_MSG_READ_AT) {
        return 0;
    }
    fs_resp_t *fr = (fs_resp_t *)(g_fsrx + sizeof(*h));
    if (fr->status != FS_OK) {
        return 0;
    }
    uint32_t got = fr->length;
    if (got > len) {
        got = len;
    }
    u_memcpy(out, g_fsrx + sizeof(*h) + sizeof(*fr), got);

    {
        /* 仅打印前 3 次读取用于确认通路；逐次打印会在播放期间频繁占用
         * 串口输出与调度时间片，本身就是卡顿来源之一。 */
        static uint32_t cnt = 0;
        if (cnt < 3) {
            cnt++;
            char db[24];
            u_print("[pa] fs_read_at #");
            u_print(u_utoa_s((uint64_t)cnt, db, sizeof(db)));
            u_print(" off=");
            u_print(u_utoa_s((uint64_t)off, db, sizeof(db)));
            u_print(" got=");
            u_print(u_utoa_s((uint64_t)got, db, sizeof(db)));
            u_print("\n");
        }
    }
    return got;
}

/* ---- 解码/播放主状态（放全局避免大栈占用） ---- */
#define INBUF_SIZE   32768              /* 输入滑动窗口（>> 最大 MP3 帧 ~1.5KB） */

static mp3dec_t g_mp3;
static uint8_t  g_inbuf[INBUF_SIZE];
static int16_t  g_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];   /* 2*1152 = 2304 */

static void print_dec(uint64_t v)
{
    char buf[24];
    u_print(u_utoa_s(v, buf, sizeof(buf)));
}

int main(int argc, char **argv)
{
    const char *fname = (argc > 1) ? argv[1] : "MOONHALO.MP3";
    /* 基准模式（exec BIN/playaudio FILE bench）：只解码不送 HDA，用于隔离
     * 解码吞吐与磁盘读取吞吐，诊断播放卡顿来源。 */
    bool bench = (argc > 2 && u_strcmp(argv[2], "bench") == 0);

    u_print("[playaudio] SukiOS MP3 player (minimp3 + Intel HDA)\n");
    u_print("[playaudio] file = ");
    u_print(fname);
    u_print("\n");

    /* 认领 FS 应答端口（退出时由内核 port_release_owner 自动释放） */
    {
        uint64_t rc = sys_port_claim(APP_PORT);
        char db[24];
        u_print("[playaudio] claim APP_PORT rc=");
        u_print(u_utoa_s(rc, db, sizeof(db)));
        u_print("\n");
    }

    mp3dec_init(&g_mp3);

    uint32_t in_filled = 0;             /* 输入环内有效字节 */
    uint32_t file_off  = 0;             /* 文件读游标 */
    bool     eof       = false;
    int      opened    = 0;
    uint32_t out_hz = 0, out_ch = 0;
    uint64_t total_samples = 0;
    uint32_t frames = 0;

    bool first_read = true;

    /* 手动跳过 ID3v2 标签：minimp3 核心 API 不自动跳过标签，文件头 "ID3"
     * 会使首帧解析失败、frame_bytes 退回整缓冲大小，进而把紧跟其后的真实
     * 音频数据一并跳过。ID3v2 总大小 = 10 字节头 + 4 字节 7bit 编码的长度。 */
    {
        uint8_t id3[10];
        if (fs_read_at(fname, 0, 10, id3) == 10 &&
            id3[0] == 'I' && id3[1] == 'D' && id3[2] == '3') {
            uint32_t tag = 10u + ((uint32_t)id3[6] << 21) |
                                 ((uint32_t)id3[7] << 14) |
                                 ((uint32_t)id3[8] << 7)  |
                                  (uint32_t)id3[9];
            file_off = tag;
            char db[24];
            u_print("[playaudio] ID3v2 tag detected, skipping ");
            u_print(u_utoa_s((uint64_t)tag, db, sizeof(db)));
            u_print(" bytes (file cursor advanced)\n");
        }
    }

    for (;;) {
        /* 1) 补满输入窗口（单次 FS 读 <=FS_DATA_MAX，循环填充到近满） */
        while (in_filled + FS_DATA_MAX <= INBUF_SIZE && !eof) {
            uint32_t n = fs_read_at(fname, file_off, FS_DATA_MAX,
                                    g_inbuf + in_filled);
            if (n == 0) {
                eof = true;
                break;
            }
            in_filled += n;
            file_off  += n;
            if (first_read) {
                first_read = false;
                char db[24];
                u_print("[playaudio] first FS read got ");
                u_print(u_utoa_s((uint64_t)n, db, sizeof(db)));
                u_print(" bytes (in_filled=");
                u_print(u_utoa_s((uint64_t)in_filled, db, sizeof(db)));
                u_print(")\n");
            }
        }
        if (in_filled == 0) {
            break;                       /* 数据耗尽，播放完成 */
        }

        /* 2) 解一帧 */
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&g_mp3, g_inbuf, (int)in_filled,
                                          g_pcm, &info);

        /* 从流前端消费 consumed 字节。
         *
         * !!! 游标不变式（曾因违反它导致"加速+乱序"重大 bug）!!!
         *   file_off 始终等于"缓冲区尾部对应的文件位置"，即下一次
         *   fs_read_at 应读取的文件偏移。填充循环里 file_off += n 已经
         *   把游标推到缓冲末尾；因此消费缓冲区【内部】的字节时，绝不能
         *   再推进 file_off——那些字节早已读入内存，再推进就会使下次补读
         *   跳过等量的文件数据（每帧 576B@192kbps 全被丢一半，流以 ~2.24x
         *   速度推进、MP3 帧被拦腰截断错位重同步，听感即"加速+前后颠倒"）。
         *
         * 仅当 consumed 超出缓冲（minimp3 要求跳过远超缓冲的元数据，如
         * ~1.9MB 的 ID3v2 标签）时，才把"超出缓冲的部分"加到 file_off 上，
         * 实现越过标签的跳跃。 */
        uint32_t consumed = (uint32_t)info.frame_bytes;
        if (consumed == 0) {
            consumed = 1;                    /* 无同步头：跳过 1 字节重新对齐 */
        }
        if (consumed >= in_filled) {
            file_off  += consumed - in_filled;   /* 只跳过缓冲之外的部分 */
            in_filled  = 0;
        } else {
            memmove(g_inbuf, g_inbuf + consumed, in_filled - consumed);
            in_filled -= consumed;
            /* file_off 不动：消费的是已在内存中的数据 */
        }

        if (samples <= 0) {
            continue;                    /* 该次仅同步/跳过，无 PCM 输出 */
        }

        /* 3) 首个有效帧确定输出格式后打开 HDA 流（bench 模式跳过） */
        if (!opened) {
            {
                char db[24];
                u_print("[playaudio] first frame: hz=");
                u_print(u_utoa_s((uint64_t)info.hz, db, sizeof(db)));
                u_print(" ch=");
                u_print(u_utoa_s((uint64_t)info.channels, db, sizeof(db)));
                u_print(" kbps=");
                u_print(u_utoa_s((uint64_t)info.bitrate_kbps, db, sizeof(db)));
                u_print("\n");
            }
            if (!bench) {
                if (sys_audio_open((uint32_t)info.hz, (uint32_t)info.channels,
                                   16) != 0) {
                    u_print("[playaudio] sys_audio_open failed "
                            "(no HDA device or unsupported format)\n");
                    return 1;
                }
                opened = 1;
                out_hz = (uint32_t)info.hz;
                out_ch = (uint32_t)info.channels;
                u_print("[playaudio] format: ");
                print_dec(out_hz);
                u_print(" Hz, ");
                print_dec(out_ch);
                u_print(" ch, 16-bit, ");
                print_dec((uint64_t)info.bitrate_kbps);
                u_print(" kbps\n[playaudio] playing...\n");
            } else {
                opened = 1;
                out_hz = (uint32_t)info.hz;
                out_ch = (uint32_t)info.channels;
                u_print("[playaudio] BENCH mode: decode only, no HDA\n");
            }
        }

        /* 4) 写内核播放环（S16LE 交错），环满则让出 CPU 重试（背压）*/
        if (!bench) {
        uint32_t bytes = (uint32_t)samples * (uint32_t)info.channels * 2u;
        uint8_t *p = (uint8_t *)g_pcm;
        uint32_t left = bytes;
        while (left > 0) {
            int64_t w = sys_audio_write(p, left);
            if (w < 0) {
                u_print("[playaudio] sys_audio_write error\n");
                left = 0;
                break;
            }
            if (w == 0) {
                sys_yield();             /* 环满：等 DMA 排空 */
                continue;
            }
            p    += w;
            left -= (uint32_t)w;
        }
        }
        /* bench 模式：累积解码 PCM 的 CRC32（全曲期望 699861505） */
        if (bench && samples > 0) {
            uint32_t pcm_bytes = (uint32_t)samples * (uint32_t)info.channels * 2u;
            g_crc = crc32((const uint8_t *)g_pcm, pcm_bytes, g_crc);
        }
        total_samples += (uint64_t)samples;
        frames++;
        if ((frames & 63) == 0) {
            char db[24];
            u_print("[playaudio] progress frames=");
            u_print(u_utoa_s((uint64_t)frames, db, sizeof(db)));
            u_print("\n");
        }
    }

    /* 5) 排空播放环后停止流 */
    if (opened) {
        while (sys_audio_queued() > 0) {
            sys_yield();
        }
        sys_audio_stop();
    }

    u_print("[playaudio] done. frames=");
    print_dec(frames);
    u_print(", samples/ch=");
    print_dec(total_samples);
    if (out_hz > 0) {
        u_print(", ~");
        print_dec(total_samples / out_hz);
        u_print(" s\n");
    } else {
        u_print("\n");
        u_print("[playaudio] no audio decoded (not a valid MP3?)\n");
    }
    if (bench) {
        char db[24];
        u_print("[playaudio] BENCH CRC32 of decoded PCM = ");
        u_print(u_utoa_s((uint64_t)g_crc, db, sizeof(db)));
        u_print("\n");
    }
    return 0;
}
