/*
 * user/lib/time.c
 * 时间相关 POSIX 函数（内核 RTC/节拍后端）。
 */
#include "libc.h"

time_t time(time_t *tloc)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) return (time_t)-1;
    if (tloc) *tloc = (time_t)tv.tv_sec;
    return (time_t)tv.tv_sec;
}

int gettimeofday(struct timeval *tv, void *tz)
{
    long r = suki_syscall2(SYS_GETTIMEOFDAY, (uint64_t)tv, (uint64_t)tz);
    return (int)libc_ret(r);
}

int clock_gettime(int clk, struct timespec *tp)
{
    long r = suki_syscall2(SYS_CLOCK_GETTIME, (uint64_t)clk, (uint64_t)tp);
    return (int)libc_ret(r);
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    long r = suki_syscall2(SYS_NANOSLEEP, (uint64_t)req, (uint64_t)rem);
    return (int)libc_ret(r);
}

unsigned int sleep(unsigned int sec)
{
    struct timespec req, rem;
    req.tv_sec = sec;
    req.tv_nsec = 0;
    while (nanosleep(&req, &rem) != 0) {
        if (errno == EINTR) { req = rem; continue; }
        return sec; /* 其他错误：返回未睡完的秒数近似为原值 */
    }
    return 0;
}

int usleep(unsigned int usec)
{
    struct timespec req;
    req.tv_sec = usec / 1000000;
    req.tv_nsec = (usec % 1000000) * 1000;
    return nanosleep(&req, NULL);
}

/* ===========================================================================
 * 日历转换（算法见 H. Hinnant "chrono" 的 civil_from_days / days_from_civil）
 * 本系统无时区数据，所有转换按 UTC 处理。
 * =========================================================================== */

static long days_from_civil(int y, int m, int d)
{
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (long)era * 146097 + doe - 719468;
}

static void civil_from_days(long z, int *py, int *pm, int *pd)
{
    z += 719468;
    int era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = z - era * 146097;
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = yoe + era * 400;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    int d = doy - (153 * mp + 2) / 5 + 1;
    int m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    *py = y; *pm = m; *pd = d;
}

static int weekday_from_days(long z)
{
    int w = (int)(z % 7);
    if (w < 0) w += 7;
    return (w + 4) % 7; /* 1970-01-01 为周四(4) */
}

struct tm *gmtime_r(const time_t *t, struct tm *r)
{
    long days = (long)(*t / 86400);
    long secs = (long)(*t % 86400);
    if (secs < 0) { secs += 86400; days -= 1; }
    int y, m, d;
    civil_from_days(days, &y, &m, &d);
    r->tm_year  = y - 1900;
    r->tm_mon   = m - 1;
    r->tm_mday  = d;
    r->tm_hour  = (int)(secs / 3600);
    r->tm_min   = (int)((secs % 3600) / 60);
    r->tm_sec   = (int)(secs % 60);
    r->tm_wday  = weekday_from_days(days);
    r->tm_yday  = (int)(days - days_from_civil(y, 1, 1));
    r->tm_isdst = 0;
    return r;
}

static struct tm g_gmtime_buf;

struct tm *gmtime(const time_t *t) { return gmtime_r(t, &g_gmtime_buf); }

struct tm *localtime_r(const time_t *t, struct tm *r)
{
    /* 无时区数据，等价于 gmtime_r */
    return gmtime_r(t, r);
}

struct tm *localtime(const time_t *t)
{
    static struct tm g_local_buf;
    return localtime_r(t, &g_local_buf);
}

time_t mktime(struct tm *tm)
{
    int y = tm->tm_year + 1900;
    int m = tm->tm_mon + 1;
    int d = tm->tm_mday;
    long days = days_from_civil(y, m, d);
    long secs = (long)tm->tm_hour * 3600 + (long)tm->tm_min * 60 + tm->tm_sec;
    time_t t = (time_t)(days * 86400 + secs);
    gmtime_r(&t, tm); /* 归一化 wday/yday，并把越界字段规范化 */
    tm->tm_isdst = 0;
    return t;
}

time_t timegm(struct tm *tm)
{
    return mktime(tm); /* 本系统无时区，timegm == mktime */
}

/* 注：difftime 返回 double，但本仓用户态工具链禁用了 SSE（general-regs-only），
 * 无法按 x86_64 ABI 经 XMM 返回浮点，故不提供 difftime。libcurl 等以自身时间处理替代。 */

static const char *g_wdays[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
static const char *g_mons[12] = {"Jan","Feb","Mar","Apr","May","Jun",
                                 "Jul","Aug","Sep","Oct","Nov","Dec"};

char *asctime(const struct tm *tm)
{
    static char buf[26];
    snprintf(buf, sizeof(buf), "%.3s %.3s%3d %.2d:%.2d:%.2d %d\n",
             g_wdays[tm->tm_wday % 7], g_mons[tm->tm_mon % 12], tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900);
    return buf;
}

char *ctime(const time_t *t)
{
    struct tm tm;
    gmtime_r(t, &tm);
    return asctime(&tm);
}

size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm)
{
    size_t out = 0;
    for (; *fmt && out < max; fmt++) {
        if (*fmt != '%') {
            if (out + 1 < max) s[out++] = *fmt;
            continue;
        }
        char tmp[32];
        const char *p = tmp;
        int n = 0;
        switch (*++fmt) {
            case 'Y': n = snprintf(tmp, sizeof(tmp), "%04d", tm->tm_year + 1900); break;
            case 'y': n = snprintf(tmp, sizeof(tmp), "%02d", (tm->tm_year + 1900) % 100); break;
            case 'm': n = snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mon + 1); break;
            case 'd': n = snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mday); break;
            case 'H': n = snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour); break;
            case 'M': n = snprintf(tmp, sizeof(tmp), "%02d", tm->tm_min); break;
            case 'S': n = snprintf(tmp, sizeof(tmp), "%02d", tm->tm_sec); break;
            case 'j': n = snprintf(tmp, sizeof(tmp), "%03d", tm->tm_yday + 1); break;
            case 'w': n = snprintf(tmp, sizeof(tmp), "%d", tm->tm_wday); break;
            case 'a': n = snprintf(tmp, sizeof(tmp), "%.3s", g_wdays[tm->tm_wday % 7]); break;
            case 'A': n = snprintf(tmp, sizeof(tmp), "%s",
                            (tm->tm_wday==0?"Sunday":tm->tm_wday==1?"Monday":
                             tm->tm_wday==2?"Tuesday":tm->tm_wday==3?"Wednesday":
                             tm->tm_wday==4?"Thursday":tm->tm_wday==5?"Friday":"Saturday")); break;
            case 'b': case 'h': n = snprintf(tmp, sizeof(tmp), "%.3s", g_mons[tm->tm_mon % 12]); break;
            case 'B': n = snprintf(tmp, sizeof(tmp), "%s",
                            (tm->tm_mon==0?"January":tm->tm_mon==1?"February":
                             tm->tm_mon==2?"March":tm->tm_mon==3?"April":
                             tm->tm_mon==4?"May":tm->tm_mon==5?"June":
                             tm->tm_mon==6?"July":tm->tm_mon==7?"August":
                             tm->tm_mon==8?"September":tm->tm_mon==9?"October":
                             tm->tm_mon==10?"November":"December")); break;
            case 'F': n = snprintf(tmp, sizeof(tmp), "%04d-%02d-%02d",
                            tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday); break;
            case 'T': n = snprintf(tmp, sizeof(tmp), "%02d:%02d:%02d",
                            tm->tm_hour, tm->tm_min, tm->tm_sec); break;
            case 'c': n = snprintf(tmp, sizeof(tmp), "%.3s %.3s%3d %02d:%02d:%02d %d",
                            g_wdays[tm->tm_wday % 7], g_mons[tm->tm_mon % 12], tm->tm_mday,
                            tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900); break;
            case 'Z': n = snprintf(tmp, sizeof(tmp), "UTC"); break;
            case 'z': n = snprintf(tmp, sizeof(tmp), "+0000"); break;
            case '%': tmp[0] = '%'; n = 1; break;
            default:  tmp[0] = '%'; tmp[1] = *fmt; n = 2; break;
        }
        if (n < 0) n = 0;
        for (int i = 0; i < n && out + 1 < max; i++) s[out++] = p[i];
    }
    if (max > 0) s[out] = 0;
    return out;
}
