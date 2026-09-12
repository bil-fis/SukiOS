/*
 * user/lib/shims/time.h
 * -----------------------------------------------------------------------------
 * SukiOS 用户态标准 <time.h> 实现头（最小子集）。
 *
 * 实现归属：user/lib/unistd.c（time/gettimeofday/clock_gettime/nanosleep）。
 * 不提供 struct tm / localtime / strftime 等日历转换（本 libc 暂未实现，
 * 避免声而无实现）；仅提供基于秒/纳秒的时间接口，满足绝大多数用户态需求。
 */
#ifndef _SUKI_SHIM_TIME_H
#define _SUKI_SHIM_TIME_H

#include <stddef.h>
#include <stdint.h>

/* 基础时间类型 */
typedef int64_t time_t;

struct timeval {
    int64_t tv_sec;   /* 秒 */
    int64_t tv_usec;  /* 微秒 */
};

struct timespec {
    int64_t tv_sec;   /* 秒 */
    int64_t tv_nsec;  /* 纳秒 */
};

/* 分解时间结构（日历转换，供 gmtime/mktime/strftime 使用） */
struct tm {
    int tm_sec;    /* 0..59 */
    int tm_min;    /* 0..59 */
    int tm_hour;   /* 0..23 */
    int tm_mday;   /* 1..31 */
    int tm_mon;    /* 0..11（0=一月） */
    int tm_year;   /* 自 1900 起年数 */
    int tm_wday;   /* 0..6（0=周日） */
    int tm_yday;   /* 0..365 */
    int tm_isdst;  /* >0 夏令时，0 否，<0 未知 */
};

/* 时钟 ID（取自 posix.h，与内核一致） */
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

/* ---- 时间接口（user/lib/time.c） ---- */
time_t time(time_t *tloc);
int    gettimeofday(struct timeval *tv, void *tz);
int    clock_gettime(int clk_id, struct timespec *tp);
int    nanosleep(const struct timespec *req, struct timespec *rem);
unsigned int sleep(unsigned int seconds);
int    usleep(unsigned int usec);

/* 日历转换（纯 UTC，本系统无时区数据） */
struct tm *gmtime_r(const time_t *t, struct tm *r);
struct tm *gmtime(const time_t *t);
struct tm *localtime_r(const time_t *t, struct tm *r);
struct tm *localtime(const time_t *t);
time_t     mktime(struct tm *tm);
time_t     timegm(struct tm *tm);
size_t     strftime(char *s, size_t max, const char *fmt, const struct tm *tm);
char      *asctime(const struct tm *tm);
char      *ctime(const time_t *t);
/* 注：difftime 返回 double，本仓用户态工具链禁用 SSE（general-regs-only）无法经 XMM
 * 返回浮点，故不提供；调用方以 (t1 - t0) 直接相减替代。 */

#endif /* _SUKI_SHIM_TIME_H */
