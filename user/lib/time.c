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
