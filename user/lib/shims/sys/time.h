/*
 * user/lib/shims/sys/time.h
 * -----------------------------------------------------------------------------
 * SukiOS 用户态标准 <sys/time.h>。struct timeval / gettimeofday 已在 <time.h> 提供，
 * 此处复用并补充 itimerval 与 timeradd/sub 宏，供需要 struct timeval 的网络代码包含。
 */
#ifndef _SUKI_SHIM_SYS_TIME_H
#define _SUKI_SHIM_SYS_TIME_H

#include <time.h>

struct itimerval {
    struct timeval it_interval;  /* 周期性间隔 */
    struct timeval it_value;     /* 当前剩余 */
};

#define timerisset(tvp)  ((tvp)->tv_sec || (tvp)->tv_usec)
#define timerclear(tvp)  do { (tvp)->tv_sec = 0; (tvp)->tv_usec = 0; } while (0)
#define timeradd(a, b, r) do { \
    (r)->tv_sec = (a)->tv_sec + (b)->tv_sec; \
    (r)->tv_usec = (a)->tv_usec + (b)->tv_usec; \
    if ((r)->tv_usec >= 1000000) { (r)->tv_sec++; (r)->tv_usec -= 1000000; } \
} while (0)
#define timersub(a, b, r) do { \
    (r)->tv_sec = (a)->tv_sec - (b)->tv_sec; \
    (r)->tv_usec = (a)->tv_usec - (b)->tv_usec; \
    if ((r)->tv_usec < 0) { (r)->tv_sec--; (r)->tv_usec += 1000000; } \
} while (0)
#define timercmp(a, b, op) \
    (((a)->tv_sec == (b)->tv_sec) ? ((a)->tv_usec op (b)->tv_usec) : ((a)->tv_sec op (b)->tv_sec))

#endif /* _SUKI_SHIM_SYS_TIME_H */
