/*
 * user/lib/shims/sys/types.h
 * -----------------------------------------------------------------------------
 * SukiOS 用户态标准 <sys/types.h>：POSIX 基础标量类型定义（单一真相源）。
 *
 * 供 libcurl 等第三方库使用。与 <unistd.h> / <sys/socket.h> / <time.h> 中的同名
 * typedef 取值严格一致（C11 允许兼容类型重复 typedef；本头用守卫宏避免重复）。
 */
#ifndef _SUKI_SHIM_SYS_TYPES_H
#define _SUKI_SHIM_SYS_TYPES_H

#include <stddef.h>
#include <stdint.h>

/* 供第三方库判断基础整型已定义（避免其自带 old-style 定义冲突） */
#ifndef __BIT_TYPES_DEFINED__
#define __BIT_TYPES_DEFINED__ 1
#endif

#ifndef _SUKI_SHIM_SSIZE_T_DEFINED
#define _SUKI_SHIM_SSIZE_T_DEFINED
typedef int64_t  ssize_t;
#endif

typedef int64_t  off_t;
typedef int64_t  loff_t;
typedef int64_t  suseconds_t;
typedef int64_t  blksize_t;
typedef int64_t  blkcnt_t;
typedef int64_t  time_t;      /* 与 <time.h> 一致（64 位有符号） */
typedef int64_t  clock_t;

typedef uint64_t dev_t;
typedef uint64_t ino_t;
typedef uint32_t nlink_t;
typedef uint32_t mode_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;
typedef uint32_t id_t;
typedef int32_t  key_t;
typedef int      pid_t;

/* socket 相关（与 <sys/socket.h> 一致） */
#ifndef _SUKI_SHIM_SOCKLEN_T_DEFINED
#define _SUKI_SHIM_SOCKLEN_T_DEFINED
typedef uint32_t socklen_t;
#endif
#ifndef _SUKI_SHIM_SA_FAMILY_T_DEFINED
#define _SUKI_SHIM_SA_FAMILY_T_DEFINED
typedef uint16_t sa_family_t;
#endif

#endif /* _SUKI_SHIM_SYS_TYPES_H */
