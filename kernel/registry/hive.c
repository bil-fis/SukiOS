/*
 * kernel/registry/hive.c
 * -----------------------------------------------------------------------------
 * SukiRegistry Hive 内核侧解析器（与 tools/registry_editor.py 同格式）。
 * 仅实现「读取 + 按路径提取值」，写入/序列化由 CONFIG_SERVER(Ring3) 负责。
 * 参见 include/kernel/registry.h 的布局说明。
 */
#include <kernel/registry.h>
#include <kernel/string.h>

/* 小端读取辅助 */
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

/* CRC32 增量更新（初始值 0xFFFFFFFF），与 Python Crc32(data, crc) 一致 */
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return crc;
}

uint32_t sukreg_crc32(const uint8_t *data, size_t len)
{
    return ~crc32_update(0xFFFFFFFFu, data, len);
}

/* 在 body 中按路径查找条目，返回数据指针；out_type/out_len 可选 */
static const uint8_t *FindEntry(const uint8_t *body, uint64_t body_len,
                                const char *path, uint32_t *out_type,
                                uint64_t *out_len)
{
    uint64_t path_len = 0;
    while (path[path_len]) path_len++;

    uint64_t off = 0;
    while (off + 12 <= body_len) {
        uint32_t type     = le32(body + off);
        uint32_t name_len = le32(body + off + 8);
        const uint8_t *name = body + off + 12;
        if (off + 12 + name_len + 8 > body_len)
            break;
        uint64_t data_len = le64(body + off + 12 + name_len);
        const uint8_t *val = body + off + 12 + name_len + 8;
        if (off + 12 + name_len + 8 + data_len > body_len)
            break;

        if (name_len == path_len && memcmp(name, path, name_len) == 0) {
            if (out_type) *out_type = type;
            if (out_len)  *out_len  = data_len;
            return val;
        }
        off += 12 + name_len + 8 + data_len;
    }
    return NULL;
}

bool RegistryParseSystem(const uint8_t *data, size_t size, system_config_t *cfg)
{
    if (!data || !cfg || size < SUKREG_HEADER_SIZE)
        return false;

    const sukreg_header_t *h = (const sukreg_header_t *)data;
    if (memcmp(h->magic, "SUKREG\0\0", 8) != 0)
        return false;
    if (h->version != SUKREG_VERSION)
        return false;

    const uint8_t *body = data + SUKREG_HEADER_SIZE;
    size_t body_len = size - SUKREG_HEADER_SIZE;

    /* CRC32 覆盖 [0..48) + body（与 Python 工具一致） */
    uint32_t calc = crc32_update(0xFFFFFFFFu, data, 48);
    calc = crc32_update(calc, body, body_len);
    calc = ~calc;
    if (calc != h->crc32)
        return false;

    memset(cfg, 0, sizeof(*cfg));

    uint32_t type;
    uint64_t len;
    const uint8_t *v;

    v = FindEntry(body, body_len, "/System/Display/Width", &type, &len);
    if (v && type == SUKREG_TYPE_INT64) {
        cfg->have_display = true;
        cfg->display_width = (uint32_t)le64(v);
    }
    v = FindEntry(body, body_len, "/System/Display/Height", &type, &len);
    if (v && type == SUKREG_TYPE_INT64) {
        cfg->have_display = true;
        cfg->display_height = (uint32_t)le64(v);
    }
    v = FindEntry(body, body_len, "/System/Display/Bpp", &type, &len);
    if (v && type == SUKREG_TYPE_INT64) {
        cfg->have_display = true;
        cfg->display_bpp = (uint32_t)le64(v);
    }
    v = FindEntry(body, body_len, "/System/Kernel/KdrEnabled", &type, &len);
    if (v && type == SUKREG_TYPE_BOOL) {
        cfg->have_kdr = true;
        cfg->kdr_enabled = (len > 0 && v[0] != 0);
    }
    v = FindEntry(body, body_len, "/System/Boot/Verbose", &type, &len);
    if (v && type == SUKREG_TYPE_BOOL) {
        cfg->have_verbose = true;
        cfg->boot_verbose = (len > 0 && v[0] != 0);
    }
    return true;
}
