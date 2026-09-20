/*
 * kernel/registry/hive.c
 * -----------------------------------------------------------------------------
 * SukiRegistry Hive v2 内核侧解析器（与 tools/registry_editor.py 同格式）。
 * 仅实现「读取 + 按路径提取值」，写入/序列化由 CONFIG_SERVER(Ring3) 负责。
 * 参见 include/kernel/registry.h 的布局说明。
 *
 * v2：磁盘 body 是「层级键树」的前序序列化。本文件实现：
 *   - 校验头（魔数/版本/CRC32/root_offset 越界）。
 *   - 按路径 "System/Display/Width" 在键树中查值（首段=根键名，
 *     中间段=子键名，末段=值名）。
 */
#include <kernel/registry.h>
#include <kernel/string.h>
#include <mm/kmalloc.h>   /* RegistryCacheSystem：缓存整份 hive 字节 */

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

/* 一个键节点解析后的视图（均为文件内偏移/指针，已做越界检查） */
typedef struct {
    uint32_t name_len;
    const uint8_t *name;
    uint32_t flags;
    uint32_t value_count;
    uint64_t values_off;    /* 第一个值的 vname_len 所在偏移 */
    uint32_t subkey_count;
    uint64_t subkeys_off;   /* subkey_count 字段所在偏移（即全部值之后） */
} key_node_t;

/* 解析位于 koff 的键节点头；越界返回 false。 */
static bool ParseKeyHeader(const uint8_t *base, uint64_t total,
                           uint64_t koff, key_node_t *k)
{
    uint64_t p = koff;
    if (p + 4 > total) return false;
    uint32_t name_len = le32(base + p); p += 4;
    if (p + name_len > total) return false;
    const uint8_t *name = base + p; p += name_len;
    if (p + 4 > total) return false;
    uint32_t flags = le32(base + p); p += 4;
    if (p + 4 > total) return false;
    uint32_t value_count = le32(base + p); p += 4;
    uint64_t values_off = p;
    /* 跳过所有值 */
    for (uint32_t i = 0; i < value_count; i++) {
        if (p + 4 > total) return false;
        uint32_t vname_len = le32(base + p); p += 4;
        if (p + vname_len > total) return false; p += vname_len;
        if (p + 4 > total) return false; p += 4;            /* vtype */
        if (p + 8 > total) return false;
        uint64_t vdata_len = le64(base + p); p += 8;
        if (p + vdata_len > total) return false;
        p += vdata_len;
    }
    uint64_t subkeys_off = p;
    if (p + 4 > total) return false;
    uint32_t subkey_count = le32(base + p); p += 4;
    k->name_len = name_len;
    k->name = name;
    k->flags = flags;
    k->value_count = value_count;
    k->values_off = values_off;
    k->subkey_count = subkey_count;
    k->subkeys_off = p;        /* subkey_count 之后即第一个子键节点 */
    return true;
}

/* 返回键节点结束偏移（含其全部子键）。失败返回 0。 */
static uint64_t SkipKey(const uint8_t *base, uint64_t total, uint64_t koff)
{
    key_node_t k;
    if (!ParseKeyHeader(base, total, koff, &k))
        return 0;
    uint64_t p = k.subkeys_off;
    for (uint32_t i = 0; i < k.subkey_count; i++) {
        uint64_t end = SkipKey(base, total, p);
        if (end == 0)
            return 0;
        p = end;
    }
    return p;
}

/* 在 parent 的子键中按名查找；找到则 *out_child 设为该子键节点偏移。 */
static bool FindSubkeyByName(const uint8_t *base, uint64_t total,
                             const key_node_t *parent,
                             const char *name, uint32_t name_len,
                             uint64_t *out_child)
{
    uint64_t p = parent->subkeys_off;
    for (uint32_t j = 0; j < parent->subkey_count; j++) {
        key_node_t sk;
        if (!ParseKeyHeader(base, total, p, &sk))
            return false;
        if (sk.name_len == name_len && memcmp(sk.name, name, name_len) == 0) {
            *out_child = p;
            return true;
        }
        uint64_t end = SkipKey(base, total, p);
        if (end == 0)
            return false;
        p = end;
    }
    return false;
}

/* 在键 k 的值表中按名查找；找到则填 out_type/out_len 并返回数据指针。 */
static const uint8_t *FindValueInKey(const uint8_t *base, uint64_t total,
                                     const key_node_t *k,
                                     const char *vname, uint32_t vname_len,
                                     uint32_t *out_type, uint64_t *out_len)
{
    uint64_t p = k->values_off;
    for (uint32_t i = 0; i < k->value_count; i++) {
        if (p + 4 > total) return NULL;
        uint32_t this_vname_len = le32(base + p); p += 4;
        if (p + this_vname_len > total) return NULL;
        const uint8_t *this_vname = base + p; p += this_vname_len;
        if (p + 4 > total) return NULL;
        uint32_t vtype = le32(base + p); p += 4;
        if (p + 8 > total) return NULL;
        uint64_t vdata_len = le64(base + p); p += 8;
        if (p + vdata_len > total) return NULL;
        const uint8_t *vdata = base + p; p += vdata_len;
        if (this_vname_len == vname_len &&
            memcmp(this_vname, vname, vname_len) == 0) {
            *out_type = vtype;
            *out_len = vdata_len;
            return vdata;
        }
    }
    return NULL;
}

/*
 * 按路径查值。path 形如 "System/Display/Width"：
 *   首段 = 根键名；中间段 = 子键名；末段 = 值名。
 * 返回数据指针，或 NULL（路径不存在/越界）。
 */
static const uint8_t *FindValueByPath(const uint8_t *base, uint64_t total,
                                      uint64_t root_off, const char *path,
                                      uint32_t *out_type, uint64_t *out_len)
{
    const char *seg[8];
    uint32_t seglen[8];
    int nseg = 0;
    const char *s = path;
    while (*s == '/') s++;
    if (*s == 0) return NULL;
    const char *start = s;
    for (;;) {
        if (*s == '/' || *s == 0) {
            if (nseg >= 8) return NULL;
            seg[nseg] = start;
            seglen[nseg] = (uint32_t)(s - start);
            nseg++;
            if (*s == 0) break;
            start = s + 1;
        }
        s++;
    }
    if (nseg < 2) return NULL;

    uint64_t cur = root_off;
    for (int i = 1; i < nseg - 1; i++) {
        key_node_t ck;
        if (!ParseKeyHeader(base, total, cur, &ck))
            return NULL;
        uint64_t child = 0;
        if (!FindSubkeyByName(base, total, &ck, seg[i], seglen[i], &child))
            return NULL;
        cur = child;
    }
    key_node_t fk;
    if (!ParseKeyHeader(base, total, cur, &fk))
        return NULL;
    return FindValueInKey(base, total, &fk, seg[nseg - 1], seglen[nseg - 1],
                          out_type, out_len);
}

/* 把 hive 中的字符串值拷入定长字段（超长安全截断，恒以 NUL 结尾）。 */
static void CopyStrField(char *dst, size_t cap, const uint8_t *src, uint64_t len)
{
    if (!dst || cap == 0)
        return;
    size_t n = ((size_t)len < cap - 1) ? (size_t)len : cap - 1;
    if (src && n)
        memcpy(dst, src, n);
    dst[n] = '\0';
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

    /* body_size 为真实 body 长度（来自 header），与 FS 读回的尾部填充解耦。 */
    uint64_t body_size = h->body_size;
    if (body_size > size - SUKREG_HEADER_SIZE)
        return false;
    const uint8_t *body = data + SUKREG_HEADER_SIZE;
    uint64_t total = SUKREG_HEADER_SIZE + body_size;

    /* CRC32 覆盖 [0..48) + body（与 Python 工具一致） */
    uint32_t calc = crc32_update(0xFFFFFFFFu, data, 48);
    calc = crc32_update(calc, body, body_size);
    calc = ~calc;
    if (calc != h->crc32)
        return false;

    uint64_t root_off = h->root_offset;
    if (root_off < SUKREG_HEADER_SIZE || root_off > total)
        return false;

    memset(cfg, 0, sizeof(*cfg));

    uint32_t type;
    uint64_t len;
    const uint8_t *v;

    /* Display 为 uint64（JSON type "uint64"），兼容 INT64/UINT64。 */
    v = FindValueByPath(data, total, root_off, "System/Display/Width", &type, &len);
    if (v && (type == SUKREG_TYPE_INT64 || type == SUKREG_TYPE_UINT64)) {
        cfg->have_display = true;
        cfg->display_width = (uint32_t)le64(v);
    }
    v = FindValueByPath(data, total, root_off, "System/Display/Height", &type, &len);
    if (v && (type == SUKREG_TYPE_INT64 || type == SUKREG_TYPE_UINT64)) {
        cfg->have_display = true;
        cfg->display_height = (uint32_t)le64(v);
    }
    v = FindValueByPath(data, total, root_off, "System/Display/Bpp", &type, &len);
    if (v && (type == SUKREG_TYPE_INT64 || type == SUKREG_TYPE_UINT64)) {
        cfg->have_display = true;
        cfg->display_bpp = (uint32_t)le64(v);
    }
    v = FindValueByPath(data, total, root_off, "System/Kernel/KdrEnabled", &type, &len);
    if (v && type == SUKREG_TYPE_BOOL) {
        cfg->have_kdr = true;
        cfg->kdr_enabled = (len > 0 && v[0] != 0);
    }
    v = FindValueByPath(data, total, root_off, "System/Boot/Verbose", &type, &len);
    if (v && type == SUKREG_TYPE_BOOL) {
        cfg->have_verbose = true;
        cfg->boot_verbose = (len > 0 && v[0] != 0);
    }

    /* ---- System/Boot：启动设备标识 + 启动画面（图标/进度条/自定义 logo）---- */
    v = FindValueByPath(data, total, root_off, "System/Boot/BootDeviceType",
                        &type, &len);
    if (v && type == SUKREG_TYPE_STRING) {
        cfg->have_boot_device_type = true;
        CopyStrField(cfg->boot_device_type, sizeof(cfg->boot_device_type), v, len);
    }
    v = FindValueByPath(data, total, root_off, "System/Boot/ShowLogo", &type, &len);
    if (v && type == SUKREG_TYPE_BOOL) {
        cfg->have_show_logo = true;
        cfg->show_logo = (len > 0 && v[0] != 0);
    }
    v = FindValueByPath(data, total, root_off, "System/Boot/ShowProgress",
                        &type, &len);
    if (v && type == SUKREG_TYPE_BOOL) {
        cfg->have_show_progress = true;
        cfg->show_progress = (len > 0 && v[0] != 0);
    }
    v = FindValueByPath(data, total, root_off, "System/Boot/BootLogoID",
                        &type, &len);
    if (v && type == SUKREG_TYPE_STRING) {
        cfg->have_boot_logo_id = true;
        CopyStrField(cfg->boot_logo_id, sizeof(cfg->boot_logo_id), v, len);
    }
    v = FindValueByPath(data, total, root_off, "System/Boot/CustomLogo/Enabled",
                        &type, &len);
    if (v && type == SUKREG_TYPE_BOOL) {
        cfg->have_custom_logo = true;
        cfg->custom_logo_enabled = (len > 0 && v[0] != 0);
    }
    v = FindValueByPath(data, total, root_off, "System/Boot/CustomLogo/Path",
                        &type, &len);
    if (v && type == SUKREG_TYPE_STRING) {
        cfg->have_custom_logo = true;
        CopyStrField(cfg->custom_logo_path, sizeof(cfg->custom_logo_path), v, len);
    }
    v = FindValueByPath(data, total, root_off, "System/Boot/CustomLogo/DirectPath",
                        &type, &len);
    if (v && type == SUKREG_TYPE_STRING) {
        cfg->have_custom_logo = true;
        CopyStrField(cfg->custom_logo_direct_path,
                     sizeof(cfg->custom_logo_direct_path), v, len);
    }
    return true;
}

/* ===========================================================================
 * 运行时缓存 + 按路径查询 + 动态值覆盖
 * ===========================================================================
 * 内核只读注册表；SYS_REGISTRY_READ 让 Ring3 也可按路径取值。缓存整份
 * system.sre 字节（前序键树），任意路径查询都直接在缓存里导航，无需重读磁盘。
 * 动态覆盖用于「注册表内只是标识、真实值由内核探测」的键：典型为
 * System/Boot/BootDeviceType —— 用户态读它时拿到的是内核实际探测结果。
 */

static uint8_t *g_reg_cache = NULL;      /* 缓存的 hive 字节（kmalloc） */
static size_t   g_reg_size = 0;          /* 缓存字节数 */

#define REG_OVERRIDE_MAX 8
static struct {
    const char *path;
    const char *value;
} g_reg_override[REG_OVERRIDE_MAX];
static int g_reg_override_n = 0;

bool RegistryCacheSystem(const uint8_t *data, size_t size)
{
    if (!data || size < SUKREG_HEADER_SIZE)
        return false;
    uint8_t *copy = (uint8_t *)kmalloc(size);
    if (!copy)
        return false;
    memcpy(copy, data, size);
    if (g_reg_cache)
        kfree(g_reg_cache);
    g_reg_cache = copy;
    g_reg_size = size;
    /* 覆盖表按新缓存作废（启动期只缓存一次，此处仅为语义严谨）。 */
    g_reg_override_n = 0;
    return true;
}

bool RegistryOverrideLookup(const char *path, const char **out_value)
{
    if (!path)
        return false;
    for (int i = 0; i < g_reg_override_n; i++) {
        const char *p = g_reg_override[i].path;
        const char *q = path;
        while (*p && *p == *q) { p++; q++; }
        if (*p == '\0' && *q == '\0') {
            if (out_value)
                *out_value = g_reg_override[i].value;
            return true;
        }
    }
    return false;
}

void RegistrySetOverrideString(const char *path, const char *value)
{
    if (!path || !value)
        return;
    for (int i = 0; i < g_reg_override_n; i++) {
        const char *p = g_reg_override[i].path;
        const char *q = path;
        while (*p && *p == *q) { p++; q++; }
        if (*p == '\0' && *q == '\0') {
            g_reg_override[i].value = value;   /* 替换（值字符串由调用方常驻持有） */
            return;
        }
    }
    if (g_reg_override_n >= REG_OVERRIDE_MAX)
        return;
    g_reg_override[g_reg_override_n].path = path;
    g_reg_override[g_reg_override_n].value = value;
    g_reg_override_n++;
}

int RegistryQuery(const char *path, uint32_t *out_type,
                  const uint8_t **out_data, uint64_t *out_len)
{
    /* 1) 动态覆盖优先：内核探测出的真实值（如 BootDeviceType） */
    {
        const char *ov = NULL;
        if (RegistryOverrideLookup(path, &ov) && ov) {
            if (out_type) *out_type = SUKREG_TYPE_STRING;
            if (out_data) *out_data = (const uint8_t *)ov;
            if (out_len)  *out_len = (uint64_t)strlen(ov);
            return 0;
        }
    }
    /* 2) 静态 hive */
    if (!g_reg_cache || g_reg_size < SUKREG_HEADER_SIZE)
        return -1;
    const sukreg_header_t *h = (const sukreg_header_t *)g_reg_cache;
    if (memcmp(h->magic, "SUKREG\0\0", 8) != 0)
        return -1;
    uint64_t body_size = h->body_size;
    if (body_size > g_reg_size - SUKREG_HEADER_SIZE)
        return -1;
    uint64_t total = SUKREG_HEADER_SIZE + body_size;
    uint64_t root_off = h->root_offset;
    if (root_off < SUKREG_HEADER_SIZE || root_off > total)
        return -1;
    uint32_t t = 0;
    uint64_t l = 0;
    const uint8_t *v = FindValueByPath(g_reg_cache, total, root_off, path, &t, &l);
    if (!v)
        return -1;
    if (out_type) *out_type = t;
    if (out_data) *out_data = v;
    if (out_len)  *out_len = l;
    return 0;
}
