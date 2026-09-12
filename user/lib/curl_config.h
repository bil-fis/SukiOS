/*
 * user/lib/curl_config.h
 * =============================================================================
 * SukiOS 专用的 libcurl 配置头（等价于对 libcurl 运行 ./configure 的产物）。
 *
 * 背景：SukiOS 用户态为 freestanding（自供 libc 桩），无法在宿主运行 libcurl
 * 的 autotools/CMake 探测，故手写本文件，由 curl_setup.h 在定义 HAVE_CONFIG_H
 * 时经 `#include "curl_config.h"` 引入（-I user/lib 命中本文件）。
 *
 * 取值原则：只声明 SukiOS 用户态 libc 真正提供的头/函数（见 user/lib/shims/、
 * user/lib/*.c），并关闭所有不需要的协议/后端，确保 libcurl 可在本仓库编译、
 * 链接与运行。
 *
 * 目标能力：HTTP / HTTPS（TLS 后端后续经 mbedTLS 提供）/ FTP / TFTP，
 * 仅 IPv4、同步 resolver（getaddrinfo）、无线程、无 c-ares、无 LDAP/SMTP/... 
 * =============================================================================
 */
#ifndef SUKIOS_CURL_CONFIG_H
#define SUKIOS_CURL_CONFIG_H

/* ---- 目标平台标识 ---- */
#define CURL_OS "x86_64-sukios"
#define CURL_EXTERN_SYMBOL

/* ---- 基础类型大小（x86_64 LP64） ---- */
#define SIZEOF_INT 4
#define SIZEOF_SHORT 2
#define SIZEOF_LONG 8
#define SIZEOF_LONG_LONG 8
#define SIZEOF_VOIDP 8
#define SIZEOF_OFF_T 8
#define SIZEOF_CURL_OFF_T 8
#define SIZEOF_CURL_SOCKET_T 4   /* curl_socket_t = int（内核 fd） */
#define SIZEOF_SIZE_T 8
#define SIZEOF_TIME_T 8

/* time_t 为有符号 64 位（见 user/lib/shims/time.h） */

/* =============================================================================
 * 1) 关闭不需要的协议 / 功能（官方 configure 开关，非桩实现）
 * ============================================================================= */
#define CURL_DISABLE_DICT 1
#define CURL_DISABLE_FILE 1        /* 无 stdio 文件流协议需求 */
#define CURL_DISABLE_GOPHER 1
#define CURL_DISABLE_IMAP 1
#define CURL_DISABLE_LDAP 1
#define CURL_DISABLE_LDAPS 1
#define CURL_DISABLE_MQTT 1
#define CURL_DISABLE_POP3 1
#define CURL_DISABLE_RTSP 1
#define CURL_DISABLE_SMTP 1
#define CURL_DISABLE_TELNET 1
#define CURL_DISABLE_WEBSOCKETS 1
#define CURL_DISABLE_DOH 1
#define CURL_DISABLE_ALTSVC 1
#define CURL_DISABLE_HSTS 1
#define CURL_DISABLE_AWS 1
#define CURL_DISABLE_HTTPSIG 1
#define CURL_DISABLE_MIME 1
#define CURL_DISABLE_FORM_API 1
#define CURL_DISABLE_NETRC 1
#define CURL_DISABLE_PROGRESS_METER 1
#define CURL_DISABLE_GETOPTIONS 1
#define CURL_DISABLE_SHA512_256 1
#define CURL_DISABLE_IPFS 1
#define CURL_DISABLE_SHUFFLE_DNS 1
#define CURL_DISABLE_BINDLOCAL 1
#define CURL_DISABLE_KERBEROS_AUTH 1
#define CURL_DISABLE_NEGOTIATE_AUTH 1

/* =============================================================================
 * 2) 系统头文件（SukiOS shims / 工具链提供）
 * ============================================================================= */
#define HAVE_ARPA_INET_H 1
#define HAVE_FCNTL_H 1
#define HAVE_NETDB_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_POLL_H 1
#define HAVE_SYS_SELECT_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define HAVE_STDBOOL_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_LIMITS_H 1
#define HAVE_ERRNO_H 1
#define HAVE_TIME_H 1
#define HAVE_DIRENT_H 1

/* 不存在的头（保持未定义/置 0） */
/* HAVE_STRINGS_H, HAVE_SYS_UN_H, HAVE_NET_IF_H, HAVE_SYS_IOCTL_H, HAVE_IO_H,
   HAVE_LOCALE_H, HAVE_PWD_H, HAVE_LIBGEN_H, HAVE_TERMIOS_H, HAVE_SYS_PARAM_H,
   HAVE_SYS_RESOURCE_H, HAVE_IFADDRS_H, HAVE_NETINET_TCP_H, HAVE_NETINET_UDP_H,
   HAVE_NETINET_IP_H, HAVE_NETINET_IN6_H, HAVE_LINUX_TCP_H, HAVE_SYS_EVENTFD_H,
   HAVE_STDATOMIC_H —— 均不定义 */

/* =============================================================================
 * 3) 可用函数 / 类型
 * ============================================================================= */
#define HAVE_FCNTL 1
#define HAVE_FCNTL_O_NONBLOCK 1
#define HAVE_GETADDRINFO 1
#define HAVE_FREEADDRINFO 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_CLOCK_GETTIME_MONOTONIC 1
#define HAVE_GMTIME_R 1
#define HAVE_LOCALTIME_R 1
#define HAVE_SOCKET 1
#define HAVE_RECV 1
#define HAVE_SEND 1
#define HAVE_POLL 1
#define HAVE_SELECT 1
#define HAVE_STRUCT_TIMEVAL 1
#define HAVE_SUSECONDS_T 1
#define HAVE_SA_FAMILY_T 1
#define HAVE_BOOL_T 1
#define HAVE_GETPPID 1
#define HAVE_GETHOSTNAME 1
#define HAVE_OPENDIR 1
#define HAVE_SIGACTION 1
#define HAVE_SIGNAL 1
#define HAVE_PIPE 1
/* 注意：libcurl 对 HAVE_* 一律用 #ifdef 判定，故「不具备」的能力必须【完全不定义】，
 * 绝不能写成 #define HAVE_X 0（那仍算已定义）。以下能力均不定义：
 *   HAVE_STRUCT_SOCKADDR_STORAGE / HAVE_SOCKADDR_IN6_SIN6_SCOPE_ID /
 *   HAVE_TIME_T_UNSIGNED / HAVE_ATOMIC / HAVE_ARC4RANDOM / ... */

/* 明确不可用（不定义 / 置 0）：ATOMIC, ARC4RANDOM, ALARM, ACCEPT4, PIPE2,
   EVENTFD, SENDMSG, SENDMMSG, STRERROR_R, GETADDRINFO_THREADSAFE,
   GETHOSTBYNAME_R, GETIFADDRS, IF_NAMETOINDEX, GETEUID, GETPWUID(_R),
   GETRLIMIT, SETRLIMIT, SIGSETJMP, SIGINTERRUPT, UTIME(S), FSEEKO,
   MEMRCHR, BASENAME, FNMATCH, REALPATH, GETHOSTBYNAME_R, SOCKETPAIR,
   MACH_ABSOLUTE_TIME, MEMSET_S, MEMSET_EXPLICIT, WRITABLE_ARGV,
   IOCTL_FIONBIO, IOCTL_SIOCGIFADDR, SETSOCKOPT_SO_NONBLOCK */

/* =============================================================================
 * 4) zlib（由 miniz 的 zlib 兼容层提供，见 user/lib/shims/zlib.h）
 * ============================================================================= */
#define HAVE_LIBZ 1

/* =============================================================================
 * 5) TLS 后端（当前未接入；后续经 mbedTLS 提供时改定义 USE_MBEDTLS）
 * ============================================================================= */
/* #define USE_MBEDTLS 1 */

#endif /* SUKIOS_CURL_CONFIG_H */
