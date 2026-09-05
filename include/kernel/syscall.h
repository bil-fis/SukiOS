/*
 * include/kernel/syscall.h
 * -----------------------------------------------------------------------------
 * 系统调用 ABI（手册第 5 章）+ 完整 POSIX 系统调用号。
 *
 * 指令：syscall（IA32_LSTAR 指向 syscall_entry）。
 * 约定：%rax=调用号；%rdi,%rsi,%rdx,%r10,%r8,%r9=参数；返回值在 %rax。
 * 内核返回用户态使用 iretq（选择子红线 0x1B/0x23 与 sysretq 的
 * 选择子布局算术不兼容，iretq 可精确指定 CS/SS）。
 *
 * 调用关系：kmain -> syscall_init()；Ring3 syscall -> syscall_entry(汇编)
 *           -> syscall_dispatch(本模块 C 分发) -> sys_* 各实现。
 *
 * 【完整号表来源】
 *   号值与共享结构体统一定义在 <sukios/posix.h>（内核与用户态唯一契约）。
 *   本头文件以 SUKI_KERNEL_BUILD 宏包含它——该宏屏蔽其中的用户态内联
 *   syscall 封装（内核不得使用 syscall 指令，且内联汇编 clobber 语义不同）。
 *   为避免既有代码（SYS_MACH_MSG 等）写法变化，本文件直接复用 posix.h
 *   中的号常量，不再重复定义。
 *
 * 【返回值约定】（与 posix.h 一致，newlib 直接可用）
 *   成功返回 >=0；失败返回 -errno（范围 -1..-4095）。
 */
#ifndef _SUKI_KERNEL_SYSCALL_H
#define _SUKI_KERNEL_SYSCALL_H

#include <kernel/types.h>

#define SUKI_KERNEL_BUILD 1
#include <sukios/posix.h>

/*
 * 号表分区说明（详见 <sukios/posix.h>）：
 *   0..19   SukiOS/Mach 原生（mach_msg、execve、audio、fork、getpid…）
 *   20..39  进程 / 调度 / 资源（waitpid、brk、rlimit…）
 *   40..89  文件与目录 I/O（open/read/write/stat/getdents…）
 *   90..99  内存映射（六参 POSIX mmap/munmap/mprotect…）
 *  100..109 时间（clock_gettime/nanosleep/gettimeofday…）
 *  110..129 系统信息（uname/sysinfo/sysconf/getrandom…）
 *  130..149 网络（号位预留）
 * 注意：SYS_MMAP(90) 是六参 POSIX mmap；旧的两参匿名映射保留为
 * SYS_MMAP_LEGACY(14)/SYS_MUNMAP_LEGACY(15)，两者语义不冲突。
 */

/* 用户态地址空间上限（含）：0x00007FFFFFFFFFFF */
#define USER_SPACE_TOP    0x00007FFFFFFFFFFFUL

/* 失败返回打包宏（内核 sys_* 实现统一使用） */
#define SYS_ERR(n)        SUKI_ERR(n)

void syscall_init(void);

/* 在当前 CPU 上写 syscall MSR 组（LSTAR/STAR/FMASK/EFER.SCE 每核私有）。
 * AP 必须在 ap_main 中调用，否则该核上的用户任务执行 syscall 即 #DF。 */
void syscall_init_cpu(void);

/*
 * 极度安全的内存操作（手册 9.1）：
 * 所有用户指针严禁直接解引用，必须经 copy_from_user/copy_to_user。
 * 返回实际拷贝字节数；用户指针越界返回 0。
 */
size_t copy_from_user(void *dest, const void *user_src, size_t n);
size_t copy_to_user(void *user_dest, const void *src, size_t n);

/* 从用户态拷贝一个 NUL 结尾的字符串到内核缓冲（最多 max 字节含 NUL）。
 * 成功返回字符串长度（不含 NUL），失败（越界/未终结/空路径）返回 -1。
 * 逐字节 copy_from_user 会非常慢，故先做一次性区间校验再整段拷贝。 */
int64_t copy_str_from_user(char *dest, const char *user_src, size_t max);

/*
 * C 分发器（由 syscall_entry.S 调用）。
 * a1..a6 对应用户态 rdi/rsi/rdx/r10/r8/r9；a6 由汇编经【栈】传入——
 * 前 6 个参数（num + a1..a5）已占满寄存器，POSIX mmap 的第 6 个用户
 * 参数只能走栈。
 */
uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5,
                          uint64_t a6);

/* SukiNative 原生对象 API 分发（130..199）：与 POSIX（20..129）平行的原生接口。
 * 当前 Phase 0 仅完成号位预留与分发路由，未实现的具体号返回 -ENOSYS；
 * Phase 1 起在此实现对象/句柄子系统（suki_object_t、句柄表、SYS_SUKI_WAIT 等）。 */
uint64_t sys_suki_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6);

/* POSIX 子系统初始化（fd 表等）。kmain 在 syscall_init 之后调用。 */
void posix_init(void);

#endif /* _SUKI_KERNEL_SYSCALL_H */
