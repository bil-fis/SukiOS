/*
 * include/kernel/registry.h
 * -----------------------------------------------------------------------------
 * SukiRegistry Hive —— 内核侧解析（与 tools/registry_editor.py 同格式）。
 *
 * 无签名方案防护模型：内核路径/端口隔离 + IPC 仲裁 + CRC32(错误检测)
 * + generation(在线防回滚) + 审计。离线篡改在无签名下不可防，CRC32 仅用于
 * 检测意外损坏，不宣称防篡改（详见「SukiOS 安全配置存储系统设计（无签名版）」）。
 *
 * 磁盘布局与 Python 工具（tools/registry_editor.py）完全一致。v2 为层级键树：
 *   [0..8)   magic  "SUKREG\0\0"
 *   [8..12)  version u32        (=2)
 *   [12..16) flags u32
 *   [16..24) root_offset u64    (根键节点在文件中的偏移，恒为 64)
 *   [24..32) entry_count u64    (v2 未使用，置 0)
 *   [32..40) generation u64
 *   [40..48) timestamp u64
 *   [48..52) crc32 u32          (覆盖 [0..48) + body 的 CRC32)
 *   [52..60) body_size u64      (v2：body 字节数，用于 CRC/解析边界)
 *   [60..64) reserved(补齐到 64)
 *   [64..)   body：根键节点（前序递归序列化的键树）。每个键节点：
 *            <name_len u32><name><flags u32><value_count u32>
 *            [值表] 每个值：<vname_len u32><vname><vtype u32><vdata_len u64><vdata>
 *            <subkey_count u32>
 *            [子键表] 每个子键：内嵌一个键节点（前序递归）
 *   查值路径形如 "System/Display/Width"：首段为根键名，中间段为子键名，末段为值名。
 */
#ifndef _SUKI_KERNEL_REGISTRY_H
#define _SUKI_KERNEL_REGISTRY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SUKREG_MAGIC        0x53554B52   /* "SUKR" */
#define SUKREG_VERSION      2
#define SUKREG_HEADER_SIZE  64

typedef struct __attribute__((packed)) {
    uint8_t  magic[8];      /* "SUKREG\0\0" */
    uint32_t version;
    uint32_t flags;
    uint64_t root_offset;
    uint64_t entry_count;
    uint64_t generation;
    uint64_t timestamp;
    uint32_t crc32;         /* 覆盖 [0..48) + body */
    uint64_t body_size;     /* v2：body 字节数（CRC 与解析边界用真实长度，避免被 FS 读回的尾部填充影响） */
    uint32_t reserved;      /* 补齐到 64 字节 */
} sukreg_header_t;

#define SUKREG_TYPE_INT64   1
#define SUKREG_TYPE_UINT64  2
#define SUKREG_TYPE_BOOL    3
#define SUKREG_TYPE_STRING  4
#define SUKREG_TYPE_BINARY  5
#define SUKREG_TYPE_LINK    6

/* 内核消费的系统配置子集（其余键由 CONFIG_SERVER 在用户态消费） */
typedef struct {
    bool     have_display;
    uint32_t display_width;
    uint32_t display_height;
    uint32_t display_bpp;
    bool     have_kdr;
    bool     kdr_enabled;
    bool     have_verbose;
    bool     boot_verbose;
} system_config_t;

/* 标准 CRC32（多项式 0xEDB88320），与 Python 工具实现逐位一致 */
uint32_t sukreg_crc32(const uint8_t *data, size_t len);

/* 解析内存中的 hive 并提取系统配置；CRC 通过返回 true，否则 false。
 * data/size 为整文件（含 64 字节头 + body）。失败时不动 cfg。 */
bool RegistryParseSystem(const uint8_t *data, size_t size, system_config_t *cfg);

#endif /* _SUKI_KERNEL_REGISTRY_H */
