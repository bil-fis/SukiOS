/*
 * user/lib/suki.h
 * -----------------------------------------------------------------------------
 * SukiOS Ring3 用户程序运行库（无 libc，自由环境）。
 *
 * 提供：syscall 封装（ABI：rax=号, rdi/rsi/rdx/r10/r8=参数）、mach_msg
 * 便捷收发、最小字符串工具。与内核共享的消息/协议布局见 include/ipc/。
 *
 * 【系统调用号的唯一真相源】
 *   号表、errno、POSIX 结构体与常量全部取自 <sukios/posix.h>——内核侧
 *   （include/kernel/syscall.h）与用户侧（本文件）都包含它，两侧绝不各自
 *   硬编码，从根本上杜绝号表漂移（历史上曾因此出现海量非法调用号）。
 *   本文件不再重复定义任何 SYS_* 号与 SUKI_* 常量。
 */
#ifndef _SUKI_USER_SUKI_H
#define _SUKI_USER_SUKI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sukios/posix.h>   /* 完整 syscall 号表 + errno + POSIX 常量/结构体 */

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

/* ---- syscall 原语 ----
 * 注意 clobber 列表须覆盖编译器可能借用的所有调用者保存寄存器：syscall 指令
 * 本身改 rcx/r11/rax/flags；若省略 r9/rbx，编译器可能把循环不变量（如恒定的
 * 调用号 n）缓存在 r9 中，而相邻的另一次 syscall 调用（如 sys_port_claim）经由
 * 寄存器分配踩坏 r9，导致后续迭代复用被污染的调用号，表现为海量非法 syscall
 * （曾致使 INPUT_SERVER 每 tick 发 KERNEL_BASE 量级调用号）。故将 r9/rbx 一并列
 * 入 clobber，强制编译器每次调用都正确重载荷号到 rax。 */
static inline uint64_t suki_syscall5(uint64_t n, uint64_t a1, uint64_t a2,
                                     uint64_t a3, uint64_t a4, uint64_t a5)
{
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = a4;
    register uint64_t r8  __asm__("r8")  = a5;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                       "r"(r10), "r"(r8)
                     : "rcx", "r11", "r9", "rbx", "memory");
    return ret;
}

/*
 * 0..4 参与 6 参版本（完整 POSIX 调用的参数个数从 0 到 6 都有：
 * getpid 0 参、open 3 参、mmap 6 参）。
 * 实现转发到 <sukios/posix.h> 的 __suki_syscallN —— 那里是 syscall 内联
 * 汇编的实现处（含 rcx/r11/rbx/r9 的完整 clobber 清单）。本文件只做命名
 * 包装，既有调用点（大量历史代码用 suki_syscall5）无需改动，也避免了
 * 两处各写一份内联汇编而可能出现的 clobber 清单不一致。
 */
static inline uint64_t suki_syscall0(uint64_t n)
{
    return (uint64_t)__suki_syscall0((int64_t)n);
}
static inline uint64_t suki_syscall1(uint64_t n, uint64_t a1)
{
    return (uint64_t)__suki_syscall1((int64_t)n, (int64_t)a1);
}
static inline uint64_t suki_syscall2(uint64_t n, uint64_t a1, uint64_t a2)
{
    return (uint64_t)__suki_syscall2((int64_t)n, (int64_t)a1, (int64_t)a2);
}
static inline uint64_t suki_syscall3(uint64_t n, uint64_t a1, uint64_t a2,
                                     uint64_t a3)
{
    return (uint64_t)__suki_syscall3((int64_t)n, (int64_t)a1, (int64_t)a2,
                                     (int64_t)a3);
}
static inline uint64_t suki_syscall4(uint64_t n, uint64_t a1, uint64_t a2,
                                     uint64_t a3, uint64_t a4)
{
    return (uint64_t)__suki_syscall4((int64_t)n, (int64_t)a1, (int64_t)a2,
                                     (int64_t)a3, (int64_t)a4);
}
static inline uint64_t suki_syscall6(uint64_t n, uint64_t a1, uint64_t a2,
                                     uint64_t a3, uint64_t a4, uint64_t a5,
                                     uint64_t a6)
{
    return (uint64_t)__suki_syscall6((int64_t)n, (int64_t)a1, (int64_t)a2,
                                     (int64_t)a3, (int64_t)a4, (int64_t)a5,
                                     (int64_t)a6);
}

/* P0-R8：ACPI S5 软关机（SYS_REBOOT mode=1）。 */
static inline void suki_poweroff(void)
{
    suki_syscall5(SYS_REBOOT, 1, 0, 0, 0, 0);
}

/* 重启（SYS_REBOOT mode=0）。 */
static inline void suki_reboot(void)
{
    suki_syscall5(SYS_REBOOT, 0, 0, 0, 0, 0);
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

/* ---- P0-5：匿名内存映射（按需分页，首次触碰才耗物理页） ----
 * 【两参旧接口】走 SYS_MMAP_LEGACY(14)/SYS_MUNMAP_LEGACY(15)。
 * 完整六参 POSIX mmap(addr,len,prot,flags,fd,off) 是 SYS_MMAP(90)，
 * 见下方 sys_mmap_posix() 封装。
 * prot 用 POSIX 语义位（SUKI_PROT_READ/SUKI_PROT_WRITE，取自 posix.h）；
 * 恒不可执行（内核 W^X 红线）。返回基址，失败 NULL。 */
static inline void *sys_mmap(uint64_t len, uint64_t prot)
{
    return (void *)suki_syscall5(SYS_MMAP_LEGACY, len, prot, 0, 0, 0);
}

/* 解除 sys_mmap 建立的映射。返回 0 成功，-1 失败。 */
static inline int sys_munmap(void *addr, uint64_t len)
{
    return (int)suki_syscall5(SYS_MUNMAP_LEGACY, (uint64_t)addr, len, 0, 0, 0);
}

/*
 * 完整六参 POSIX mmap（SYS_MMAP = 90）。
 * 第 4 个参数必须经 r10 传递（syscall 指令占用 rcx 保存返回 RIP），
 * 第 6 个参数走 r9 —— 故此处需要 7 个寄存器，单独写一个 6 参封装
 * （suki_syscall5 只支持 5 个用户参数）。
 * 返回映射基址；失败返回 (void *)-1（即 MAP_FAILED）。
 */
static inline void *sys_mmap_posix(void *addr, uint64_t len, int prot,
                                   int flags, int fd, int64_t off)
{
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = (uint64_t)flags;
    register uint64_t r8  __asm__("r8")  = (uint64_t)(int64_t)fd;
    register uint64_t r9  __asm__("r9")  = (uint64_t)off;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"((uint64_t)SYS_MMAP), "D"(addr), "S"(len),
                       "d"(prot), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "rbx", "memory");
    return (void *)ret;
}

/* 完整 POSIX munmap（SYS_MUNMAP = 91）。成功返回 0，失败 -1。 */
static inline int sys_munmap_posix(void *addr, uint64_t len)
{
    return (int)suki_syscall5(SYS_MUNMAP, (uint64_t)addr, len, 0, 0, 0);
}

/* 非阻塞读 COM1 控制台输入：返回 0..255 的 ASCII 字节；无数据时返回 -1。
 * 供 INPUT_SERVER 把串口作为控制台输入源（headless QEMU 经 -serial 注入）。 */
static inline long sys_serial_read(void)
{
    return (long)suki_syscall5(SYS_SERIAL_READ, 0, 0, 0, 0, 0);
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
int     u_memcmp(const void *a, const void *b, size_t n);
char   *strchr(const char *s, int c);              /* FatFs ff.c 非法字符检查用 */

/* FatFs（drivers/FatFs/ff.c）在 FF_USE_LFN>=1 时申请的 LFN 工作缓冲；
 * 单线程 FS_SERVER 用简易页粒度堆实现（见 suki.c）。 */
void   *ff_memalloc(unsigned int msize);
void    ff_memfree(void *mblock);

/* 标准 C 名字的内存原语（供第三方库如 minimp3 链接；实现见 suki.c）。
 * -ffreestanding 下编译器也可能对结构体/数组拷贝隐式生成 memcpy/memset
 * 调用，故所有用户程序都提供这些强符号更稳妥。 */
void  *memcpy(void *d, const void *s, size_t n);
void  *memset(void *d, int c, size_t n);
void  *memmove(void *d, const void *s, size_t n);
void   u_print(const char *s);                  /* debug_write 便捷版 */
void   u_printn(const char *s, size_t n);

/*
 * udbg_printf —— 用户态调试输出，受编译期 CONFIG_DEBUG_SERIAL 控制：
 *   - make run（DBG=0）：CONFIG_DEBUG_SERIAL=0，展开为空，零开销/零刷屏；
 *   - make run-dbg（DBG=1）：CONFIG_DEBUG_SERIAL=1，等价于 u_print。
 * 仅用于「逐次/详细」诊断；显示服务等组件的关键状态（如视频模式开/关、
 * 桌面合成完成）应改用无条件 u_print，确保默认 run 下用户也能看到服务就绪。
 * 需 USER_CFLAGS 含 -include build/config.h（本 Makefile 已加）。
 */
#if defined(CONFIG_DEBUG_SERIAL) && CONFIG_DEBUG_SERIAL
  #define udbg_printf(...) u_print(__VA_ARGS__)
#else
  #define udbg_printf(...) ((void)0)
#endif
char  *u_utoa_s(uint64_t v, char *buf, size_t size); /* 十进制，长度安全：
                                                 * 至多写 size-1 字符 + NUL，
                                                 * size>=21 永不截断 */
char  *u_utoa(uint64_t v, char *buf);           /* 兼容包装(=u_utoa_s(v,buf,24))，
                                                 * 缓冲须 >=24 字节；新代码勿用 */

#endif /* _SUKI_USER_SUKI_H */
