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
#define SYS_TASK_SPAWN    1
#define SYS_TASK_EXIT     2
#define SYS_YIELD         3
#define SYS_DEBUG_WRITE   4
#define SYS_INPUT_READ    5
#define SYS_REBOOT        6
#define SYS_PORT_CLAIM    7
#define SYS_EXECVE        8
#define SYS_WAIT          9
#define SYS_AUDIO_OPEN    10
#define SYS_AUDIO_WRITE   11
#define SYS_AUDIO_QUEUED  12
#define SYS_AUDIO_STOP    13
#define SYS_MMAP          14
#define SYS_MUNMAP        15

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
#define APP_PORT        8               /* 独立 app 可认领的通用应答端口 */

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

/* 加载并执行 ELF 映像，替换当前进程（内核从 FS 读取 path 指向的文件） */
static inline int sys_execve(const char *path, char *const argv[],
                             char *const envp[])
{
    return (int)suki_syscall5(SYS_EXECVE, (uint64_t)path,
                               (uint64_t)argv, (uint64_t)envp, 0, 0);
}

/* 新建一个 Ring3 子任务并装载 ELF（不替换当前进程）。内核从 FS 读取
 * path 指向的文件。成功返回子任务 PID（>=0），失败返回 -1。
 * 语义类 Unix fork+exec：父任务应随后调用 sys_wait(pid) 等待其结束。 */
static inline int sys_task_spawn(const char *path, char *const argv[],
                                 char *const envp[])
{
    return (int)suki_syscall5(SYS_TASK_SPAWN, (uint64_t)path,
                               (uint64_t)argv, (uint64_t)envp, 0, 0);
}

/* 阻塞等待 PID=pid 的子任务退出，返回其退出码。pid 非本任务子进程时返回 -1。 */
static inline uint64_t sys_wait(uint64_t pid)
{
    return suki_syscall5(SYS_WAIT, pid, 0, 0, 0, 0);
}

/* ---- 音频（HDA 内核驱动，类 ATA 内核态特例） ---- */
/* 打开 PCM 输出流：成功返回 0，参数不支持/无设备返回 -1 */
static inline int sys_audio_open(uint32_t rate, uint32_t channels, uint32_t bits)
{
    return (int)suki_syscall5(SYS_AUDIO_OPEN, rate, channels, bits, 0, 0);
}

/* 写 PCM（S16LE 交错）到内核播放环。返回实际接受字节数（0=环满，应
 * sys_yield 后重试）；未打开/无设备返回 (uint64_t)-1。 */
static inline int64_t sys_audio_write(const void *buf, uint64_t len)
{
    return (int64_t)suki_syscall5(SYS_AUDIO_WRITE, (uint64_t)buf, len, 0, 0, 0);
}

/* 返回环中尚未播放的字节数（用于排空等待） */
static inline uint64_t sys_audio_queued(void)
{
    return suki_syscall5(SYS_AUDIO_QUEUED, 0, 0, 0, 0, 0);
}

/* 停止并复位输出流 */
static inline void sys_audio_stop(void)
{
    suki_syscall5(SYS_AUDIO_STOP, 0, 0, 0, 0, 0);
}

/* ---- P0-5：匿名内存映射（按需分页，首次触碰才耗物理页） ---- */
/* prot bit0=可写；恒不可执行（内核 W^X 红线）。返回基址，失败 NULL。 */
#define SUKI_PROT_READ   0
#define SUKI_PROT_WRITE  1
static inline void *sys_mmap(uint64_t len, uint64_t prot)
{
    return (void *)suki_syscall5(SYS_MMAP, len, prot, 0, 0, 0);
}

/* 解除 sys_mmap 建立的映射。返回 0 成功，-1 失败。 */
static inline int sys_munmap(void *addr, uint64_t len)
{
    return (int)suki_syscall5(SYS_MUNMAP, (uint64_t)addr, len, 0, 0, 0);
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

/* 标准 C 名字的内存原语（供第三方库如 minimp3 链接；实现见 suki.c）。
 * -ffreestanding 下编译器也可能对结构体/数组拷贝隐式生成 memcpy/memset
 * 调用，故所有用户程序都提供这些强符号更稳妥。 */
void  *memcpy(void *d, const void *s, size_t n);
void  *memset(void *d, int c, size_t n);
void  *memmove(void *d, const void *s, size_t n);
void   u_print(const char *s);                  /* debug_write 便捷版 */
void   u_printn(const char *s, size_t n);
char  *u_utoa_s(uint64_t v, char *buf, size_t size); /* 十进制，长度安全：
                                                 * 至多写 size-1 字符 + NUL，
                                                 * size>=21 永不截断 */
char  *u_utoa(uint64_t v, char *buf);           /* 兼容包装(=u_utoa_s(v,buf,24))，
                                                 * 缓冲须 >=24 字节；新代码勿用 */

#endif /* _SUKI_USER_SUKI_H */
