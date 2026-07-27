/*
 * user/apps/audiotest.c
 * ---------------------------------------------------------------------------
 * HDA 路径隔离测试：播放一段已知频率(440Hz)的正弦波(S16LE 立体声 48kHz)，
 * 经 SYS_AUDIO_* 送入内核 HDA 驱动。配合 QEMU wav 后端录制，做频谱分析即可
 * 判定 HDA 链路本身是否干净（隔离解码器/磁盘/MP3 的嫌疑）。
 *
 * 用法：exec BIN/audiotest
 */
#include "lib/suki.h"

#define RATE 96000
#define CH   2
#define BITS 16
#define FREQ 440
#define DURATION_SEC 6
#define TABLE_N 1024

/* 不依赖 libm 的自实现正弦（泰勒展开 + 区间规约），double 仅在 Ring3 用 */
static double my_sin(double x)
{
    /* 规约到 [-pi, pi] */
    const double pi = 3.141592653589793;
    double twopi = 2.0 * pi;
    while (x >  pi) x -= twopi;
    while (x < -pi) x += twopi;
    double xx = x * x;
    /* sin(x) ≈ x - x^3/6 + x^5/120 - x^7/5040 */
    double r = x * (1.0 - xx / 6.0 + xx * xx / 120.0 - xx * xx * xx / 5040.0);
    return r;
}

static int16_t g_table[TABLE_N];
static int16_t g_buf[2048];

int main(void)
{
    /* 预生成正弦表 */
    for (int i = 0; i < TABLE_N; i++) {
        double ang = (double)i / TABLE_N * 6.283185307179586;
        double v = my_sin(ang) * 28000.0;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        g_table[i] = (int16_t)v;
    }

    if (sys_audio_open(RATE, CH, BITS) != 0) {
        u_print("[audiotest] sys_audio_open failed\n");
        return 1;
    }
    u_print("[audiotest] open 48000/2/16, playing 440Hz sine 6s\n");

    uint32_t total_bytes = RATE * CH * (BITS/8) * DURATION_SEC;
    uint32_t written = 0;
    uint32_t phase = 0;                       /* 0..TABLE_N-1 的定点相位 */
    uint32_t per_sample = (uint32_t)((uint64_t)FREQ * TABLE_N / RATE);

    while (written < total_bytes) {
        uint32_t n_samples = sizeof(g_buf) / (CH * sizeof(int16_t));
        int16_t *p = g_buf;
        for (uint32_t i = 0; i < n_samples; i++) {
            int16_t s = g_table[phase & (TABLE_N - 1)];
            phase += per_sample;
            p[2*i+0] = s;
            p[2*i+1] = s;
        }
        uint8_t *bp = (uint8_t *)g_buf;
        uint32_t left = n_samples * CH * sizeof(int16_t);
        while (left > 0) {
            int64_t w = sys_audio_write(bp, left);
            if (w < 0) {
                u_print("[audiotest] write error\n");
                left = 0; break;
            }
            if (w == 0) {
                sys_yield();
                continue;
            }
            bp += w;
            left -= (uint32_t)w;
            written += (uint32_t)w;
        }
    }

    while (sys_audio_queued() > 0) {
        sys_yield();
    }
    sys_audio_stop();
    u_print("[audiotest] done\n");
    return 0;
}
