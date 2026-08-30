/*
 * kernel/display_cfg.c
 * -----------------------------------------------------------------------------
 * 显示服务运行时配置解析（configs/display.cfg，经 GRUB module2 加载）。
 *
 * 解析策略（防御性、零信任硬件/引导器返回值）：
 *   - 模块缺失（cfg_phys==0 或 size==0）：保留默认 1280x720 视频模式。
 *   - 文本越界：逐字节扫描，遇 NUL 或超出 cfg_size 即停，绝不读越界内存。
 *   - 行解析：取 "key = value"，忽略前后空格、# 注释、空行；
 *     key/value 各设上限（KEY_MAX/VAL_MAX），避免任意长度写入栈缓冲。
 *   - video_mode：仅 "on"/"off"（大小写不敏感）有效，其它值保留默认。
 *   - width/height：要求全部为十进制数字，且落在 (0, 8192] 范围，否则保留默认。
 *
 * 解析结果写入 g_display，供 framebuffer.c（video_mode 开关）与
 * syscall SYS_FRAMEBUFFER_MAP（向 display_server 报告画布尺寸）共享。
 */
#include <kernel/display_cfg.h>
#include <kernel/multiboot2.h>
#include <kernel/serial.h>

display_config_t g_display = {
    .video_mode = true,
    .width      = DISPLAY_DEFAULT_WIDTH,
    .height     = DISPLAY_DEFAULT_HEIGHT,
    .parsed     = false,
};

/* 显示服务接管帧缓冲标志：false=内核直接写屏/串口；true=文本转交显示服务。
 * 由 display_set_active()（SYS_DISPLAY_READY 处理体）置位，细节见头文件注释。 */
bool g_display_active = false;

void display_set_active(void)
{
    g_display_active = true;
    serial_writestr("[display] active: kernel console text now routed to display server\n");
}

/* 常量（避免依赖不确定头文件） */
#ifndef PHYS_TO_VIRT
/* 内核线性映射：物理地址 + KERNEL_BASE 即内核虚拟地址（见 vmm 常量）。 */
#define PHYS_TO_VIRT(pa) ((void *)((uint64_t)(pa) + 0xFFFF800000000000ULL))
#endif

#define KEY_MAX 32
#define VAL_MAX 32

/* 小写化用于大小写不敏感比较（不修改原串，返回静态缓冲） */
static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* 内核内简易十进制打印（避免依赖用户态 u_utoa_s） */
static void serial_write_u32(uint32_t v)
{
    char tmp[12];
    int i = 0;
    if (v == 0) { serial_writestr("0"); return; }
    while (v > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0) {
        char ch[2] = { tmp[--i], 0 };
        serial_writestr(ch);
    }
}

/* 解析单个 "key = value" 行；返回 true 表示成功识别并应用了某个已知键 */
static bool parse_kv(const char *key, const char *val)
{
    const char *expect;
    int mi;  /* match index */

    /* 依次匹配 known keys */
    struct { const char *name; int id; } tbl[] = {
        {"video_mode", 1},
        {"width",      2},
        {"height",     3},
    };
    int id = 0;
    for (mi = 0; mi < 3; mi++) {
        expect = tbl[mi].name;
        int j = 0;
        bool same = true;
        while (expect[j] && key[j]) {
            char a = key[j], b = expect[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { same = false; break; }
            j++;
        }
        if (same && expect[j] == 0 && key[j] == 0) { id = tbl[mi].id; break; }
    }
    if (id == 0) return false;

    if (id == 1) {
        /* video_mode: on/off，大小写不敏感 */
        if ((val[0] == 'o' || val[0] == 'O') && (val[1] == 'n' || val[1] == 'N')
            && (val[2] == 0 || is_space(val[2]) || val[2] == '#')) {
            g_display.video_mode = true;
        } else if ((val[0] == 'o' || val[0] == 'O')
                   && (val[1] == 'f' || val[1] == 'F')
                   && (val[2] == 'f' || val[2] == 'F')
                   && (val[3] == 0 || is_space(val[3]) || val[3] == '#')) {
            g_display.video_mode = false;
        }
        return true;
    }

    /* width/height：纯十进制数字，范围 (0, 8192] */
    uint64_t num = 0;
    int d = 0;
    while (val[d] >= '0' && val[d] <= '9') {
        num = num * 10 + (uint64_t)(val[d] - '0');
        if (num > 8192) break;   /* 超限，后面再判 */
        d++;
    }
    /* 数字后必须是结束/空格/注释，且确实解析到至少一个数字 */
    if (d == 0) return true;     /* 非法数字：保留默认 */
    char after = val[d];
    if (!(after == 0 || is_space(after) || after == '#')) return true;
    if (num == 0 || num > 8192) return true;  /* 越界：保留默认 */
    if (id == 2) g_display.width  = (uint32_t)num;
    else         g_display.height = (uint32_t)num;
    return true;
}

void display_cfg_parse(uint64_t cfg_phys, uint32_t cfg_size)
{
    if (cfg_phys == 0 || cfg_size == 0) {
        serial_writestr("[display-cfg] no module loaded; using default "
                        "1280x720 video mode\n");
        return;
    }
    if (cfg_size > 65536) cfg_size = 65536;  /* 防御：配置不应超 64KB */

    const char *p = (const char *)PHYS_TO_VIRT(cfg_phys);
    const char *end = p + cfg_size;

    char key[KEY_MAX] = {0}, val[VAL_MAX] = {0};
    const char *line = p;

    while (line < end) {
        /* 取一行（遇 NUL 或 '\n' 结束） */
        const char *lend = line;
        while (lend < end && *lend != '\n' && *lend != 0) lend++;
        /* 解析该行 */
        const char *s = line;
        while (s < lend && is_space(*s)) s++;
        if (s < lend && *s != '#') {
            /* 读 key 到第一个 '=' 或空格 */
            int ki = 0;
            while (s < lend && ki < KEY_MAX - 1 && *s != '='
                   && !is_space(*s)) {
                key[ki++] = *s++;
            }
            key[ki] = 0;
            /* 跳过空格与 '=' */
            while (s < lend && (is_space(*s) || *s == '=')) s++;
            /* 读 val 直到行尾/空格/# */
            int vi = 0;
            while (s < lend && vi < VAL_MAX - 1 && !is_space(*s)
                   && *s != '#') {
                val[vi++] = *s++;
            }
            val[vi] = 0;
            if (ki > 0) parse_kv(key, val);
        }
        if (lend >= end || *lend == 0) break;
        line = lend + 1;  /* 下一行 */
    }

    g_display.parsed = true;
    serial_writestr("[display-cfg] parsed: video_mode=");
    serial_writestr(g_display.video_mode ? "on" : "off");
    serial_writestr(" width=");
    serial_write_u32(g_display.width);
    serial_writestr(" height=");
    serial_write_u32(g_display.height);
    serial_writestr("\n");
}
