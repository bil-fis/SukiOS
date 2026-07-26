/*
 * user/lib/suki.h
 * -----------------------------------------------------------------------------
 * SukiOS Ring3 用户程序运行库（无 libc，自由环境）。
 *
 * 提供：syscall 封装（ABI：rax=号, rdi/rsi/rdx/r10/r8=参数）、mach_msg
 * 便捷收发、最小字符串工具。与内核共享的消息/协议布局见 include/ipc/。
 */
#ifndef _SUKI_USER_SUKI_H
#define _SUKI_USER_SUKI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---- 系统调用号（与 include/kernel/syscall.h 一致） ---- */
#define SYS_MACH_MSG      0
#define SYS_TASK_CREATE   1
#define SYS_TASK_EXIT     2
#define SYS_YIELD         3
#define SYS_DEBUG_WRITE   4
#define SYS_INPUT_READ    5
#define SYS_REBOOT        6
#define SYS_PORT_CLAIM    7

/* ---- mach_msg ABI（与 include/ipc/port.h 一致） ---- */
#define MACH_SEND_MSG   0x1
#define MACH_RECV_MSG   0x2
#define MACH_MSG_SUCCESS 0
#define MACH_MSGH_BITS_OOL (1U << 31)

#define DISK_PORT       1
#define FS_PORT         2
#define DISPLAY_PORT    3
#define INPUT_PORT      4
#define CONSOLE_PORT    5
#define SHELL_PORT      6
#define FS_REPLY_PORT   7

typedef struct mach_msg_header {
    uint32_t msgh_bits;
    uint32_t msgh_size;
    uint32_t msgh_remote_port;
    uint32_t msgh_local_port;
    uint32_t msgh_id;
    uint32_t msgh_reserved;
} mach_msg_header_t;

/* ---- syscall 原语 ---- */
static inline uint64_t suki_syscall5(uint64_t n, uint64_t a1, uint64_t a2,
                                     uint64_t a3, uint64_t a4, uint64_t a5)
{
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = a4;
    register uint64_t r8  __asm__("r8")  = a5;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline void sys_debug_write(const char *s, uint64_t len)
{
    suki_syscall5(SYS_DEBUG_WRITE, (uint64_t)s, len, 0, 0, 0);
}

static inline void sys_yield(void)
{
    suki_syscall5(SYS_YIELD, 0, 0, 0, 0, 0);
}

__attribute__((noreturn)) static inline void sys_exit(uint64_t code)
{
    suki_syscall5(SYS_TASK_EXIT, code, 0, 0, 0, 0);
    for (;;) { }
}

/* 认领端口的接收权（A2 项：IPC 能力） */
static inline uint64_t sys_port_claim(uint32_t port)
{
    return suki_syscall5(SYS_PORT_CLAIM, (uint64_t)port, 0, 0, 0, 0);
}

static inline uint64_t mach_msg_send(void *msg, uint32_t size)
{
    return suki_syscall5(SYS_MACH_MSG, (uint64_t)msg, MACH_SEND_MSG,
                         size, 0, 0);
}

static inline uint64_t mach_msg_recv(void *buf, uint32_t limit, uint32_t port)
{
    return suki_syscall5(SYS_MACH_MSG, (uint64_t)buf, MACH_RECV_MSG,
                         0, limit, port);
}

/* ---- 最小工具函数（user/lib/suki.c） ---- */
size_t u_strlen(const char *s);
int    u_strcmp(const char *a, const char *b);
int    u_strncmp(const char *a, const char *b, size_t n);
void  *u_memcpy(void *d, const void *s, size_t n);
void  *u_memset(void *d, int c, size_t n);
void   u_print(const char *s);                  /* debug_write 便捷版 */
void   u_printn(const char *s, size_t n);
char  *u_utoa(uint64_t v, char *buf);           /* 十进制，返回 buf */

#endif /* _SUKI_USER_SUKI_H */
