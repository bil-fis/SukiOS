/*
 * include/kernel/console.h
 * -----------------------------------------------------------------------------
 * 统一内核控制台：所有输出同时镜像到 COM1 串口，并输出到主显示
 * （帧缓冲图形控制台；若无帧缓冲则回退 VGA 文本模式）。
 * 提供最小 printf 实现供全内核使用。
 */
#ifndef _SUKI_KERNEL_CONSOLE_H
#define _SUKI_KERNEL_CONSOLE_H

#include <kernel/types.h>

void console_init(void);       /* 在 fb/vga 初始化之后调用 */
void kputc(char c);
void kputs(const char *s);
void kprintf(const char *fmt, ...);

/* 用户态控制台输出（Ring3 经 sys_debug_write 调用）：直接写帧缓冲 +
 * 串口，绕过内核诊断开关，保证 shell/UI 文本始终落在图形终端。 */
void user_puts(const char *s);

/* 用户态 TTY 输出（任意 Ring3 程序写 fd 1/2 经内核 TTY 后端 tty_write 调用）。
 * 与 user_puts 的区别：user_puts 是 shell/UI 经 sys_debug_write 主动打印的文本
 * （只走帧缓冲+串口，不进任何管道）；本函数是「普通程序写 stdout/stderr」的输出。
 * 显示服务接管帧缓冲后，这类输出【不写帧缓冲】（避免覆盖合成桌面），而是捕获进
 * 独立的「用户 TTY 环形管道」，供 shell（作为终端）经 SYS_TTY_READ 读回并渲染进
 * 自己的终端窗口；串口恒写（headless 可观测、与历史行为一致）。 */
void user_tty_out(const char *s);

/* 控制内核诊断是否镜像到帧缓冲。进入用户态服务前由 kmain 调
 * console_set_fb_diag(false)，此后内核运行期日志只走串口，帧缓冲专供
 * Ring3 shell，避免按键时被 [ipc]/[sched] 等调试日志刷屏。 */
void console_set_fb_diag(bool on);

/* 启动期(第三步系统初始化)是否把内核日志镜像到帧缓冲：仅 -v/--verbose 时为真。
 * 由 kmain 解析 GRUB 命令行设置；串口恒定输出。 */
extern bool g_boot_verbose;

/*
 * 早期控制台环形管道（early-console ring pipe）。
 * ---------------------------------------------------------------------------
 * 设计（用户需求）：显示服务接管帧缓冲后，内核诊断（原写 fbcon 的内容）
 * 不再直接写屏（否则覆盖显示服务合成桌面），而是被「捕获」进此内核环形
 * 缓冲，保留完整启动日志，供后续用户态 shell 经 SYS_CONSOLE_READ 读回
 * （类似 Unix dmesg / 内核 ring buffer）。
 *
 * 生产关系：
 *   - 生产者：kputc() 在 g_display_active 为真时，把本来要送 fbcon 的字符
 *     额外写入 g_console_pipe（串口始终照常输出，便于无图形调试）。
 *   - 消费者：sys_console_read()（SYS_CONSOLE_READ 处理体）把管道内累积的
 *     字符拷贝到用户态缓冲（user_ptr 经 copy_to_user 校验），供 shell 显示。
 *   - 环形缓冲用独立自旋锁 g_pipe_lock 保护（与 kprintf 的 g_kp_lock 解耦，
 *     避免 syscall 路径与 kprintf 路径互相死锁）。
 */
#define CONSOLE_PIPE_SIZE  16384   /* 16KB 环形缓冲，足够容纳启动期全部日志 */

size_t console_pipe_read(char *dst, size_t max);   /* 返回实际拷贝字节数 */
size_t console_pipe_avail(void);                    /* 当前可读取字节数 */

/*
 * 用户 TTY 环形管道（user TTY ring pipe）。
 * ---------------------------------------------------------------------------
 * 与内核日志管道（g_console_pipe）相互独立：本管道只装「Ring3 程序经 fd 1/2 写
 * TTY」的输出，供 shell 作为终端渲染到自己的窗口；不混入内核诊断日志。
 *
 * 生产关系：
 *   - 生产者：tty_write() -> user_tty_out() 在 g_display_active 为真时写入
 *     g_user_tty_pipe（串口始终照常输出，便于无图形调试）。
 *   - 消费者：sys_tty_read()（SYS_TTY_READ 处理体）把管道内累积的字符拷贝到
 *     用户态缓冲（user_ptr 经 copy_to_user 校验），由 shell 渲染进终端窗口。
 *   - 环形缓冲用独立自旋锁 g_utty_lock 保护（与内核日志管道 g_pipe_lock 解耦）。
 *   - 写满覆盖最旧字符（与内核日志管道一致），保证最新文本不丢、且不会因缓冲
 *     满而阻塞写者（TTY 写路径无背压、无死锁）。
 */
#define USER_TTY_PIPE_SIZE  16384   /* 16KB 环形缓冲 */
size_t user_tty_pipe_read(char *dst, size_t max);   /* 返回实际拷贝字节数 */
size_t user_tty_pipe_avail(void);                    /* 当前可读取字节数 */

/* 致命错误：输出诊断信息到串口与显示，随后停机（手册 8.1） */
__attribute__((noreturn)) void panic(const char *fmt, ...);

/*
 * dbg_printf —— 受编译期开关 CONFIG_DEBUG_SERIAL 控制的「冗长诊断日志」宏。
 *
 * 设计意图（用户诉求：串口 PIO 输出极慢，完整 POSIX 层上线后一次
 * open/read/write 即一次 IPC 往返，shell+fs-server+posixtest 并发时每秒
 * 数百条 IPC，逐条打印会刷爆串口、严重拖慢系统、淹没真正有用的输出）。
 *
 *   make run       -> CONFIG_DEBUG_SERIAL=0（默认）：dbg_printf 编译为空，
 *                     不产生任何串口流量，系统全速运行。
 *   make run-dbg   -> CONFIG_DEBUG_SERIAL=1：dbg_printf 展开为 kprintf，
 *                     输出全部冗长诊断（IPC 收发逐条追踪、disk-srv 每消息
 *                     收发细节、console-srv IPC 回显等）。
 *
 * 注意：真正的关键事件（端口耗尽、OOL 窗口耗尽、启动 FATAL、#PF/panic）
 * 不走本开关，始终无条件 kprintf，保证生产环境也能看到致命信息。
 * CONFIG_DEBUG_SERIAL 由 Makefile 注入 build/config.h（与 CONFIG_SMP 同机制）。
 */
#if defined(CONFIG_DEBUG_SERIAL) && CONFIG_DEBUG_SERIAL
#define dbg_printf(...)  kprintf(__VA_ARGS__)
#else
#define dbg_printf(...)  ((void)0)
#endif

#endif /* _SUKI_KERNEL_CONSOLE_H */
