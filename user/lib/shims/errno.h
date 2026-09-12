/*
 * user/lib/shims/errno.h
 * -----------------------------------------------------------------------------
 * SukiOS 用户态标准 <errno.h> 实现头。
 *
 * errno 是进程内全局（单线程用户态任务，每任务独立镜像即可）。
 * 错误码值直接取自内核侧 <sukios/posix.h> 的 SUKI_E* 宏（两侧值一致，
 * 绝不各自硬编码），本文件仅做名字映射 + 提供 errno 外部变量声明。
 */
#ifndef _SUKI_SHIM_ERRNO_H
#define _SUKI_SHIM_ERRNO_H

#include <sukios/posix.h>   /* SUKI_E* 错误码常量（唯一真相源） */

/* errno 变量（定义于 user/lib/errno.c，__errno_location 亦在此） */
extern int errno;
extern int *__errno_location(void);

/* 错误码名 -> SUKI_E* 值（与内核一致） */
#define EPERM            SUKI_EPERM
#define ENOENT           SUKI_ENOENT
#define ESRCH            SUKI_ESRCH
#define EINTR            SUKI_EINTR
#define EIO              SUKI_EIO
#define ENXIO            SUKI_ENXIO
#define E2BIG            SUKI_E2BIG
#define ENOEXEC          SUKI_ENOEXEC
#define EBADF            SUKI_EBADF
#define ECHILD           SUKI_ECHILD
#define EAGAIN           SUKI_EAGAIN
#define ENOMEM           SUKI_ENOMEM
#define EACCES           SUKI_EACCES
#define EFAULT           SUKI_EFAULT
#define EBUSY            SUKI_EBUSY
#define EEXIST           SUKI_EEXIST
#define EXDEV            SUKI_EXDEV
#define ENODEV           SUKI_ENODEV
#define ENOTDIR          SUKI_ENOTDIR
#define EISDIR           SUKI_EISDIR
#define EINVAL           SUKI_EINVAL
#define ENFILE           SUKI_ENFILE
#define EMFILE           SUKI_EMFILE
#define ENOTTY           SUKI_ENOTTY
#define EFBIG            SUKI_EFBIG
#define ENOSPC           SUKI_ENOSPC
#define ESPIPE           SUKI_ESPIPE
#define EROFS            SUKI_EROFS
#define EMLINK           SUKI_EMLINK
#define EPIPE            SUKI_EPIPE
#define EDOM             SUKI_EDOM
#define ERANGE           SUKI_ERANGE
#define EDEADLK          SUKI_EDEADLK
#define ENAMETOOLONG     SUKI_ENAMETOOLONG
#define ENOLCK           SUKI_ENOLCK
#define ENOSYS           SUKI_ENOSYS
#define ENOTEMPTY        SUKI_ENOTEMPTY
#define ELOOP            SUKI_ELOOP
#define ENOSR            SUKI_ENOSR
#define EILSEQ           SUKI_EILSEQ
#define ENOTSOCK         SUKI_ENOTSOCK
#define EMSGSIZE         SUKI_EMSGSIZE
#define EPROTONOSUPPORT  SUKI_EPROTONOSUPPORT
#define EOPNOTSUPP       SUKI_EOPNOTSUPP
#define ECONNREFUSED     SUKI_ECONNREFUSED
#define ETIMEDOUT        SUKI_ETIMEDOUT
#define EHOSTUNREACH     SUKI_EHOSTUNREACH
#define EALREADY         SUKI_EALREADY
#define EINPROGRESS      SUKI_EINPROGRESS
#define ECANCELED        SUKI_ECANCELED

/* 网络/套接字相关（libcurl 等第三方库需要；取值同内核 SUKI_E*） */
#define EWOULDBLOCK      SUKI_EAGAIN      /* SukiOS 与 EAGAIN 同值 */
#define EADDRINUSE       SUKI_EADDRINUSE
#define EADDRNOTAVAIL    SUKI_EADDRNOTAVAIL
#define EAFNOSUPPORT     SUKI_EAFNOSUPPORT
#define EISCONN          SUKI_EISCONN
#define ECONNABORTED     SUKI_ECONNABORTED
#define ECONNRESET       SUKI_ECONNRESET
#define EDESTADDRREQ     SUKI_EDESTADDRREQ
#define EHOSTDOWN        SUKI_EHOSTDOWN
#define ENETDOWN         SUKI_ENETDOWN
#define ENETRESET        SUKI_ENETRESET
#define ENETUNREACH      SUKI_ENETUNREACH
#define ENOBUFS          SUKI_ENOBUFS
#define ENOTCONN         SUKI_ENOTCONN
#define ENOPROTOOPT      SUKI_ENOPROTOOPT
#define EPFNOSUPPORT     SUKI_EPFNOSUPPORT
#define EPROTOTYPE       SUKI_EPROTOTYPE
#define EPROTO           SUKI_EPROTO
#define ESHUTDOWN        SUKI_ESHUTDOWN
#define ESOCKTNOSUPPORT  SUKI_ESOCKTNOSUPPORT
#define EOVERFLOW        SUKI_EOVERFLOW
#define EBADMSG          SUKI_EBADMSG
#define EBADR            SUKI_EBADR
#define EDATA            SUKI_ENODATA
#define ENODATA          SUKI_ENODATA
#define ENOMSG           SUKI_ENOMSG
#define ENOSTR           SUKI_ENOSTR
#define ETIME            SUKI_ETIME
#define EIDRM            SUKI_EIDRM
#define EMULTIHOP        SUKI_EMULTIHOP
#define ENOTBLK          SUKI_ENOTBLK
#define ENOLINK          SUKI_ENOLCK      /* 近似映射（内核未单列 ENOLINK） */

#endif /* _SUKI_SHIM_ERRNO_H */
