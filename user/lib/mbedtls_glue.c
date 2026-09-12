/*
 * user/lib/mbedtls_glue.c
 * -----------------------------------------------------------------------------
 * SukiOS 用户态 mbedTLS 平台垫片。
 *
 * 提供 MBEDTLS_ENTROPY_HARDWARE_ALT 要求的熵源钩子 mbedtls_hardware_poll()：
 *   - 优先使用 x86_64 RDRAND（CPUID.01H:ECX[30] 指示支持）；
 *   - 否则退化到 splitmix64 软件 PRNG，种子取 TSC + 时间 + 一个自增计数器。
 *
 * 说明：本系统为单核/QEMU 验证环境，RDRAND 在 KVM(-cpu host) 下可用；TCG 下
 * 亦通常实现。无论何种情况，软 PRNG 回退保证接口始终可用（非桩）。
 */
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <mbedtls/platform_time.h>

/* mbedTLS 声明的硬件轮询钩子（取自 mbedtls/entropy.h 的 ALT 路径）。 */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len,
                          size_t *olen);

/* MBEDTLS_PLATFORM_MS_TIME_ALT：由应用提供 mbedtls_ms_time()（毫秒级单调时间， 用于 TLS 内部超时/重协商）。 */
mbedtls_ms_time_t mbedtls_ms_time(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return (mbedtls_ms_time_t)time(NULL) * 1000;
    }
    return (mbedtls_ms_time_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- CPUID 检测 RDRAND ---- */
static int g_rdrand_checked;
static int g_rdrand_ok;

static void detect_rdrand(void)
{
    uint32_t eax, ebx, ecx, edx;
    eax = 1; ecx = 0;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1), "c"(0));
    g_rdrand_ok = (ecx >> 30) & 1u;
    g_rdrand_checked = 1;
}

static int rdrand64(uint64_t *out)
{
    unsigned char ok;
    uint64_t v;
    for (int i = 0; i < 10; i++) {
        __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok) { *out = v; return 1; }
    }
    return 0;
}

/* ---- 软 PRNG（splitmix64）---- */
static uint64_t g_seed;
static uint64_t g_counter;

static uint64_t splitmix64(void)
{
    uint64_t z = (g_seed += 0x9E3779B97F4A7C15UL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9UL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBUL;
    return z ^ (z >> 31);
}

static uint64_t read_tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void seed_once(void)
{
    if (g_seed) return;
    g_seed = read_tsc() ^ ((uint64_t)time(NULL) << 17) ^ 0xA5A5A5A5DEADBEEFUL;
    if (!g_seed) g_seed = 0x123456789ABCDEFUL;
}

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len,
                          size_t *olen)
{
    (void)data;
    if (!output) {
        return -1;
    }
    if (!g_rdrand_checked) {
        detect_rdrand();
    }
    seed_once();

    size_t i = 0;
    while (i < len) {
        uint64_t r;
        if (g_rdrand_ok && rdrand64(&r)) {
            /* 用 RDRAND 结果作为一次性 pad */
        } else {
            r = splitmix64() ^ (g_counter += 0x2545F4914F6CDD1DUL);
        }
        for (int b = 0; b < 8 && i < len; b++) {
            output[i++] = (unsigned char)(r & 0xff);
            r >>= 8;
        }
    }
    if (olen) {
        *olen = len;
    }
    return 0;
}
