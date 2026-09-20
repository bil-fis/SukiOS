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

/* 字符串字段容量（含 NUL）。注册表值超长时安全截断。 */
#define SUKREG_BOOTDEVTYPE_MAX   16
#define SUKREG_LOGOID_MAX        64
#define SUKREG_LOGO_PATH_MAX     192

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

    /* ---- System/Boot：启动行为与启动画面 ---- */
    bool     have_boot_device_type;
    char     boot_device_type[SUKREG_BOOTDEVTYPE_MAX];  /* 注册表内的「标识」（实际设备由内核探测，见下） */
    bool     have_show_logo;
    bool     show_logo;                                  /* ShowLogo：是否绘制启动图标 */
    bool     have_show_progress;
    bool     show_progress;                              /* ShowProgress：是否绘制进度条 */
    bool     have_boot_logo_id;
    char     boot_logo_id[SUKREG_LOGOID_MAX];            /* BootLogoID：内嵌启动图标标识（如 sukios_boot_temp_ver） */
    bool     have_custom_logo;
    bool     custom_logo_enabled;                        /* CustomLogo/Enabled */
    char     custom_logo_path[SUKREG_LOGO_PATH_MAX];     /* CustomLogo/Path：经 VFS 读取 */
    char     custom_logo_direct_path[SUKREG_LOGO_PATH_MAX]; /* CustomLogo/DirectPath：指定驱动器直读 */
} system_config_t;

/* 标准 CRC32（多项式 0xEDB88320），与 Python 工具实现逐位一致 */
uint32_t sukreg_crc32(const uint8_t *data, size_t len);

/* 解析内存中的 hive 并提取系统配置；CRC 通过返回 true，否则 false。
 * data/size 为整文件（含 64 字节头 + body）。失败时不动 cfg。 */
bool RegistryParseSystem(const uint8_t *data, size_t size, system_config_t *cfg);

/* ---------------------------------------------------------------------------
 * 运行时注册表缓存 + 按路径查询（供内核子系统与 Ring3（SYS_REGISTRY_READ）读值）
 * ---------------------------------------------------------------------------
 * 内核只读：写入/序列化由 CONFIG_SERVER(Ring3) 负责。
 * Stage3LoadConfigAndKdr() 在解析 system.sre 成功后调用 RegistryCacheSystem()
 * 保留整份 hive 字节（kmalloc 拷贝），此后任意子系统可按路径 "A/B/C" 取值。
 */

/* 缓存一份 system.sre 字节（内部拷贝，调用方随后可释放自己的缓冲）。
 * 成功返回 true；内存不足返回 false（此后 RegistryQuery 一律查不到值）。 */
bool RegistryCacheSystem(const uint8_t *data, size_t size);

/* 按路径查值（路径形如 "System/Boot/ShowLogo"）。找到返回 0 并填出参
 * （out_type 为 SUKREG_TYPE_*，out_data 指向缓存内部，只读，out_len 为字节数）；
 * 未找到/未缓存返回 -1。out_* 允许传 NULL。 */
int RegistryQuery(const char *path, uint32_t *out_type,
                  const uint8_t **out_data, uint64_t *out_len);

/* 运行时「动态值覆盖」：某些键的真实取值由内核探测决定（如 System/Boot/
 * BootDeviceType —— 注册表内只是标识，实际由内核探测并把真实结果返回给
 * 任何读它的进程）。覆盖值以 NUL 结尾字符串形式返回（type=SUKREG_TYPE_STRING），
 * 优先级高于 hive 内静态值。再次设置同路径即替换。 */
void RegistrySetOverrideString(const char *path, const char *value);

/* 查询路径是否被动态覆盖（有则 *out_value 指向覆盖字符串）。 */
bool RegistryOverrideLookup(const char *path, const char **out_value);

#endif /* _SUKI_KERNEL_REGISTRY_H */
