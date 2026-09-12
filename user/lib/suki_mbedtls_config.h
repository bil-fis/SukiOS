/*
 * user/lib/suki_mbedtls_config.h
 * =============================================================================
 * SukiOS 用户态 mbedTLS 配置（经 -DMBEDTLS_CONFIG_FILE='"suki_mbedtls_config.h"'
 * 由 mbedtls/build_info.h 引入）。
 *
 * 做法：先包含 mbedTLS 官方默认配置，再按 SukiOS freestanding 用户态的客观能力
 * 裁剪平台相关项（无 /dev/urandom、无文件型 PSA 存储、无 mbedTLS 自带网络/计时层）。
 * =============================================================================
 */
#ifndef SUKIOS_MBEDTLS_CONFIG_H
#define SUKIOS_MBEDTLS_CONFIG_H

#include "mbedtls/mbedtls_config.h"

/* ---- 熵源：不使用平台 /dev/urandom，改用硬件轮询钩子 mbedtls_hardware_poll ----
 * （实现在 user/lib/mbedtls_glue.c：RDRAND 优先，退化到 TSC+时间软 PRNG） */
#undef MBEDTLS_PLATFORM_ENTROPY
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* ---- 毫秒计时：平台分支（__unix__/Windows）在交叉目标下都不匹配，改用 ALT 钩子 ----
 * （实现在 user/lib/mbedtls_glue.c：clock_gettime(CLOCK_MONOTONIC)） */
#define MBEDTLS_PLATFORM_MS_TIME_ALT

/* ---- 关闭 SukiOS 不需要/不适配的层 ---- */
#undef MBEDTLS_NET_C        /* TLS 走 libcurl 自己的 socket（自定义 BIO） */
#undef MBEDTLS_TIMING_C     /* DTLS 计时层；TLS 客户端不需要 */
#undef MBEDTLS_FS_IO        /* 证书由 libcurl 以内存缓冲提供，无需文件 API */

/* ---- PSA 持久化存储（文件型 ITS）在 SukiOS 无对应后端，关闭；会话密钥驻留内存 ---- */
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_ITS_FILE_C

#endif /* SUKIOS_MBEDTLS_CONFIG_H */
