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

/* 控制内核诊断是否镜像到帧缓冲。进入用户态服务前由 kmain 调
 * console_set_fb_diag(false)，此后内核运行期日志只走串口，帧缓冲专供
 * Ring3 shell，避免按键时被 [ipc]/[sched] 等调试日志刷屏。 */
void console_set_fb_diag(bool on);

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
