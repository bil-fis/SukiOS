/*
 * include/kernel/rtc.h
 * -----------------------------------------------------------------------------
 * CMOS 实时时钟（MC146818）读取 —— POSIX CLOCK_REALTIME 的墙上时间来源。
 *
 * 背景：内核已有 clock_monotonic_ns()（自启动起的单调时间，源自 TSC/HPET），
 * 但 POSIX 的 time()/clock_gettime(CLOCK_REALTIME)/gettimeofday() 必须返回
 * 【墙上时间】（自 Unix Epoch 1970-01-01 起的秒数）。单调时钟无法提供它，
 * 故需要一次性读取 RTC 作为「启动时刻基准」，之后 realtime = 基准 + 单调。
 *
 * 硬件要点（OSDev RTC 条目）：
 *   - 端口 0x70 = 索引/地址寄存器（bit7=1 时禁止 NMI，读取期间必须保持）；
 *     端口 0x71 = 数据寄存器。
 *   - 寄存器 0x0A 的 UIP 位（bit7）为 1 表示「正在更新」，此时读出的
 *     秒/分/时/日期可能是撕裂的中间值 —— 必须先等待 UIP 清零再读。
 *   - 寄存器 0x0B 决定数据格式：DM(bit2)=0 -> BCD；=1 -> 二进制。
 *     12/24 小时制由 bit1 决定（=0 为 12 小时制，PM 由小时字节 bit7 表示）。
 *   - 年只有两位（0..99）：按「69..99 -> 19xx，0..68 -> 20xx」惯例映射，
 *     与 Linux/PC 固件传统一致。
 *
 * 防御性：所有读出的 BCD/二进制字段都做范围校验（月 1..12、日 1..31、
 * 时 0..23、分/秒 0..59），任一越界即判读取失败并返回 0 —— RTC 电池耗尽
 * 或 QEMU 异常时不得让「垃圾时间」污染 POSIX 时间轴。
 *
 * 调用关系：kernel/syscall/sys_posix.c -> rtc_boot_unix_ns()。
 */
#ifndef _SUKI_KERNEL_RTC_H
#define _SUKI_KERNEL_RTC_H

#include <kernel/types.h>

/* 读取 RTC 当前时间并换算为「自 Unix Epoch 起的纳秒数」。
 * 成功返回 >0 的值；失败（UIP 超时 / 字段越界 / 硬件无响应）返回 0。
 * 可重复调用（每次都重新读硬件），但生产路径只在启动时调用一次作为基准。 */
uint64_t rtc_read_unix_ns(void);

/* POSIX 时间轴初始化：在系统启动早期（clock_init 之后）调用一次，
 * 记录 (RTC 基准 - 启动单调时刻)，此后 rtc_posix_now_ns() 即可
 * 以「基准 + 当前单调时间」给出墙上时间，无需每次访问慢速 RTC 端口。 */
void rtc_time_init(void);

/* 取当前 POSIX 墙上时间（纳秒）。未初始化成功时退化为单调时间
 * （保证时间轴始终单调前进，绝不返回 0 造成应用死循环）。 */
uint64_t rtc_posix_now_ns(void);

/*
 * 直接设定「墙上时间基准」（base = 目标墙上时间 - 当前单调时间）。
 * 供 clock_settime(CLOCK_REALTIME) / settimeofday 使用：调用后所有
 * REALTIME 查询立即基于新基准，真正生效（不是空操作）。
 * 失败条件：目标时间早于当前单调时间（无法用「基准 + 单调」表达），
 * 此时保持原基准不变。返回 true 表示已生效。
 */
bool rtc_set_boot_unix(uint64_t base_ns);

/* 基准是否有效（rtc_time_init 成功读到 RTC） */
bool rtc_time_valid(void);

#endif /* _SUKI_KERNEL_RTC_H */
