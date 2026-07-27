/*
 * include/kernel/syscall.h
 * -----------------------------------------------------------------------------
 * 系统调用 ABI（手册第 5 章）。
 *
 * 指令：syscall（IA32_LSTAR 指向 syscall_entry）。
 * 约定：%rax=调用号；%rdi,%rsi,%rdx,%r10,%r8,%r9=参数；返回值在 %rax。
 * 内核返回用户态使用 iretq（选择子红线 0x1B/0x23 与 sysretq 的
 * 选择子布局算术不兼容，iretq 可精确指定 CS/SS）。
 *
 * 调用关系：kmain -> syscall_init()；Ring3 syscall -> syscall_entry(汇编)
 *           -> syscall_dispatch(本模块 C 分发)。
 */
#ifndef _SUKI_KERNEL_SYSCALL_H
#define _SUKI_KERNEL_SYSCALL_H

#include <kernel/types.h>

/* 系统调用号（手册 5.2 最小集 + 调试扩展） */
#define SYS_MACH_MSG      0
#define SYS_TASK_SPAWN    1   /* 新建 Ring3 任务并装载 ELF（类 fork+exec，不替换自身） */
#define SYS_TASK_EXIT     2
#define SYS_YIELD         3
#define SYS_DEBUG_WRITE   4   /* 扩展：调试输出(buf,len)，走 copy_from_user */
#define SYS_INPUT_READ    5   /* 扩展：取原始键盘扫描码（INPUT_SERVER 专用） */
#define SYS_REBOOT        6   /* 扩展：8042 复位重启 */
#define SYS_PORT_CLAIM    7   /* 扩展：用户态认领端口 recv 权（A2 项） */
#define SYS_EXECVE        8   /* 扩展：加载并执行 ELF 映像（替换当前进程） */
#define SYS_WAIT          9   /* 扩展：阻塞等待子任务退出并返回其退出码 */
#define SYS_AUDIO_OPEN    10  /* 扩展：打开 HDA 输出流(rate,channels,bits) */
#define SYS_AUDIO_WRITE   11  /* 扩展：写 PCM(buf,len)，返回接受字节数 */
#define SYS_AUDIO_QUEUED  12  /* 扩展：返回环中未播放字节数 */
#define SYS_AUDIO_STOP    13  /* 扩展：停止并复位输出流 */
#define SYS_MMAP          14  /* P0-5：匿名映射 mmap(len, prot)，返回基址或 0 */
#define SYS_MUNMAP        15  /* P0-5：munmap(addr, len)，返回 0 成功 */
#define SYSCALL_MAX       16

/* 用户态地址空间上限（含）：0x00007FFFFFFFFFFF */
#define USER_SPACE_TOP    0x00007FFFFFFFFFFFUL

void syscall_init(void);

/*
 * 极度安全的内存操作（手册 9.1）：
 * 所有用户指针严禁直接解引用，必须经 copy_from_user/copy_to_user。
 * 返回实际拷贝字节数；用户指针越界返回 0。
 */
size_t copy_from_user(void *dest, const void *user_src, size_t n);
size_t copy_to_user(void *user_dest, const void *src, size_t n);

/* C 分发器（由 syscall_entry.S 调用） */
uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5);

#endif /* _SUKI_KERNEL_SYSCALL_H */
