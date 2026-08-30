/*
 * user/lib/errno.c
 * 线程/进程内的 errno 全局变量。SukiOS 当前为单线程用户态任务模型，
 * 每个任务独有独立的地址空间镜像，全局 errno 在其镜像内私有，无需 TLS。
 */
#include "libc.h"

int errno = 0;

int *__errno_location(void)
{
    return &errno;
}
