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

/* 时钟 ID（取自 posix.h，与内核一致） */
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

/* ---- 时间接口（user/lib/unistd.c / syscalls.c） ---- */
time_t time(time_t *tloc);
int    gettimeofday(struct timeval *tv, void *tz);
int    clock_gettime(int clk_id, struct timespec *tp);
int    nanosleep(const struct timespec *req, struct timespec *rem);
unsigned int sleep(unsigned int seconds);
int    usleep(unsigned int usec);

#endif /* _SUKI_SHIM_TIME_H */
