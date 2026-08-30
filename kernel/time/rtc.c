/*
 * kernel/time/rtc.c
 * -----------------------------------------------------------------------------
 * CMOS 实时时钟（MC146818）驱动 —— POSIX 墙上时间基准。
 *
 * 为什么需要：clock_monotonic_ns() 给出的是「自启动起的单调时间」，而
 * time()/clock_gettime(CLOCK_REALTIME)/gettimeofday()/文件时间戳必须是
 * 自 1970-01-01 起的墙上时间。本模块在启动时读一次 RTC 作为基准，之后
 * 墙上时间 = 基准 + 单调时间（避免每次 syscall 都去访问慢速 ISA 端口）。
 *
 * 端口协议（OSDev RTC 条目）：
 *   0x70  索引寄存器（bit7=1 表示同时禁止 NMI；整个读取过程须保持置位，
 *         否则时钟更新可能打断多字节读取产生撕裂值）
 *   0x71  数据寄存器
 *   寄存器：0x00 秒 0x02 分 0x04 时 0x06 星期 0x07 日 0x08 月 0x09 年
 *          0x0A 状态A（bit7=UIP 更新进行中） 0x0B 状态B（bit2=DM 数据模式，
 *          bit1=24/12 小时制）
 *
 * 读取纪律（关键，决定时间是否可信）：
 *   1) 先读状态A，若 UIP=1 则等待其清零（有超时上限，绝不无限自旋 ——
 *      RTC 电池没电/硬件不存在时 UIP 可能永久为 1）；
 *   2) 读完全部字段后【再读一次秒】，若与首次不一致说明更新跨越了读取
 *      过程，整体重读（标准双读校验，最多重试若干次）；
 *   3) 按状态B的 DM 位决定 BCD/二进制解码；
 *   4) 全部字段做范围校验，任一越界即判失败。
 *
 * 调用关系：kmain -> rtc_time_init()；sys_posix.c -> rtc_posix_now_ns()。
 */
#include <kernel/rtc.h>
#include <kernel/clock.h>     /* clock_monotonic_ns */
#include <kernel/console.h>
#include <kernel/io.h>

#define RTC_ADDR_PORT   0x70
#define RTC_DATA_PORT   0x71

#define RTC_REG_SECONDS  0x00
#define RTC_REG_MINUTES  0x02
#define RTC_REG_HOURS    0x04
#define RTC_REG_WEEKDAY  0x06
#define RTC_REG_DAY      0x07
#define RTC_REG_MONTH    0x08
#define RTC_REG_YEAR     0x09
#define RTC_REG_STATUS_A 0x0A
#define RTC_REG_STATUS_B 0x0B

#define RTC_A_UIP        0x80   /* 更新进行中 */
#define RTC_B_DM         0x04   /* 0=BCD, 1=二进制 */
#define RTC_B_24H        0x02   /* 1=24 小时制 */
#define RTC_B_PM         0x80   /* 12 小时制下的下午标志（小时字节 bit7） */

/* UIP 等待上限（约 2 秒量级的端口轮询；超限即判硬件无响应） */
#define RTC_UIP_SPIN_MAX   2000000

/* 基准时间轴：boot_unix_ns = RTC 基准 - 启动时的单调时间 */
static uint64_t g_boot_unix_ns = 0;
static uint64_t g_boot_mono_ns = 0;
static bool     g_time_valid = false;

/* 读取 RTC 一个寄存器（全程保持 NMI 禁止位） */
static uint8_t rtc_read_reg(uint8_t reg)
{
    outb(RTC_ADDR_PORT, (uint8_t)(reg | 0x80));   /* bit7: 禁止 NMI */
    return inb(RTC_DATA_PORT);
}

/* BCD -> 二进制 */
static inline uint8_t bcd_to_bin(uint8_t v)
{
    return (uint8_t)(((v >> 4) * 10) + (v & 0x0F));
}

/* 判断 year（0..99）对应的四位年：69..99 -> 19xx，0..68 -> 20xx */
static inline int rtc_full_year(uint8_t y2)
{
    return (y2 >= 69) ? (1900 + y2) : (2000 + y2);
}

/* 每个月的天数（非闰年） */
static const int days_in_month[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

static inline bool is_leap(int y)
{
    return ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
}

/* 把 (年,月,日) 换算为「自 1970-01-01 起的天数」。
 * 经典 civil_from_days 逆变换（Howard Hinnant 算法），避免依赖 libc。 */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= (m <= 2) ? 1 : 0;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);          /* [0, 399] */
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

/*
 * 读取一次完整的 RTC 日期时间（含双读校验）。
 * 成功返回 true，out 各字段已做范围校验。
 */
static bool rtc_read_once(int *year, int *mon, int *day,
                          int *hour, int *min, int *sec)
{
    /* 1) 等 UIP 清零 */
    for (uint32_t i = 0; i < RTC_UIP_SPIN_MAX; i++) {
        if (!(rtc_read_reg(RTC_REG_STATUS_A) & RTC_A_UIP)) {
            break;
        }
        if (i + 1 == RTC_UIP_SPIN_MAX) {
            return false;      /* 超时：硬件无响应 */
        }
    }

    uint8_t status_b = rtc_read_reg(RTC_REG_STATUS_B);
    bool bcd = !(status_b & RTC_B_DM);
    bool h24 = (status_b & RTC_B_24H) != 0;

    uint8_t s1 = rtc_read_reg(RTC_REG_SECONDS);
    uint8_t mi = rtc_read_reg(RTC_REG_MINUTES);
    uint8_t hh = rtc_read_reg(RTC_REG_HOURS);
    uint8_t dd = rtc_read_reg(RTC_REG_DAY);
    uint8_t mo = rtc_read_reg(RTC_REG_MONTH);
    uint8_t yy = rtc_read_reg(RTC_REG_YEAR);
    uint8_t s2 = rtc_read_reg(RTC_REG_SECONDS);

    /* 2) 双读校验：更新跨越读取过程则本次作废（调用方重试） */
    if (s1 != s2) {
        return false;
    }

    if (bcd) {
        s1 = bcd_to_bin(s1);
        mi = bcd_to_bin(mi);
        dd = bcd_to_bin(dd);
        mo = bcd_to_bin(mo);
        yy = bcd_to_bin(yy);
        /* 小时字段特殊：12 小时制时 bit7 是 PM 标志，只有低位是 BCD */
        hh = (uint8_t)(bcd_to_bin((uint8_t)(hh & 0x7F)) | (hh & RTC_B_PM));
    }

    int hour_i;
    if (h24) {
        hour_i = hh & 0x7F;
    } else {
        bool pm = (hh & RTC_B_PM) != 0;
        hour_i = hh & 0x7F;                 /* 12 小时制：1..12 */
        if (hour_i == 12) {
            hour_i = 0;                     /* 12 AM -> 0, 12 PM -> 12 */
        }
        if (pm) {
            hour_i += 12;
        }
    }
    int year_i = rtc_full_year(yy);
    int mon_i = mo;
    int day_i = dd;

    /* 3) 全字段范围校验（防御：电池耗尽时 RTC 返回 0xFF 等垃圾值） */
    if (mon_i < 1 || mon_i > 12) {
        return false;
    }
    if (day_i < 1) {
        return false;
    }
    {
        int dim = days_in_month[mon_i - 1];
        if (mon_i == 2 && is_leap(year_i)) {
            dim = 29;
        }
        if (day_i > dim) {
            return false;
        }
    }
    if (hour_i < 0 || hour_i > 23) {
        return false;
    }
    if (mi > 59 || s1 > 59) {
        return false;
    }
    if (year_i < 1970 || year_i > 2100) {
        return false;
    }

    *year = year_i;
    *mon = mon_i;
    *day = day_i;
    *hour = hour_i;
    *min = mi;
    *sec = s1;
    return true;
}

uint64_t rtc_read_unix_ns(void)
{
    int year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0;
    bool ok = false;
    /* 最多重试 8 轮：每轮都做 UIP 等待 + 双读校验 + 范围校验 */
    for (int attempt = 0; attempt < 8 && !ok; attempt++) {
        ok = rtc_read_once(&year, &mon, &day, &hour, &min, &sec);
    }
    if (!ok) {
        return 0;
    }
    int64_t days = days_from_civil(year, (unsigned)mon, (unsigned)day);
    int64_t secs = days * 86400 + hour * 3600 + min * 60 + sec;
    if (secs < 0) {
        return 0;
    }
    return (uint64_t)secs * 1000000000ULL;
}

void rtc_time_init(void)
{
    uint64_t rtc_ns = rtc_read_unix_ns();
    uint64_t mono_ns = clock_monotonic_ns();
    if (rtc_ns == 0) {
        /* RTC 不可用：不伪造墙上时间。以「单调时间」作为退化基准（等价于
         * 把启动时刻当作 Epoch），并记 valid=false 供诊断；时间轴仍然单调
         * 前进，POSIX 应用不会因时间倒流而异常。 */
        g_boot_unix_ns = 0;
        g_boot_mono_ns = 0;
        g_time_valid = false;
        kprintf("[rtc] CMOS RTC unavailable or invalid; REALTIME falls back "
                "to monotonic (boot = epoch)\n");
        return;
    }
    if (rtc_ns > mono_ns) {
        g_boot_unix_ns = rtc_ns - mono_ns;
        g_boot_mono_ns = 0;
    } else {
        /* clock_monotonic_ns 已大于 RTC（理论上不会发生）：取 0 基准 */
        g_boot_unix_ns = 0;
        g_boot_mono_ns = 0;
    }
    g_time_valid = true;

    uint64_t now = rtc_posix_now_ns();
    kprintf("[rtc] CMOS RTC ok: boot_unix=%lus, now=%lus\n",
            (unsigned long)(g_boot_unix_ns / 1000000000ULL),
            (unsigned long)(now / 1000000000ULL));
}

uint64_t rtc_posix_now_ns(void)
{
    /* 墙上时间 = 基准 + 单调时间。g_boot_mono_ns 固定为 0（基准在启动时
     * 已折算），保留该变量是为将来「单调时间含休眠」语义预留。 */
    return g_boot_unix_ns + (clock_monotonic_ns() - g_boot_mono_ns);
}

bool rtc_set_boot_unix(uint64_t base_ns)
{
    /* base_ns 是「墙上时间基准」：realtime = base + mono。
     * 校验目标时刻不早于当前单调时间，否则基准将为负（无法表达）。 */
    uint64_t mono = clock_monotonic_ns();
    if (base_ns + mono < mono) {
        return false;                 /* 溢出：拒绝 */
    }
    g_boot_unix_ns = base_ns;
    g_boot_mono_ns = 0;
    g_time_valid = true;              /* 既然显式设定，时间轴即为有效 */
    return true;
}

bool rtc_time_valid(void)
{
    return g_time_valid;
}
