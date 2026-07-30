/*
 * include/kernel/klog.h
 * -----------------------------------------------------------------------------
 * P0-R3a：早期启动日志内存环缓冲。
 *
 * 动机：串口是「易失输出」——若宿主端未及时挂接（或后期串口被占用/失效），
 * 早期启动日志将永久丢失。本模块把流经 serial_write 的每个字符同步镜像进
 * 一块 64KB 静态环缓冲（覆盖式），提供三种取回途径：
 *   1) klog_dump()            —— 内核内主动重放到串口（panic 前手动调用等）；
 *   2) gdbstub `monitor klog` —— GDB 附着后经 qRcmd 远程取回（崩溃后取证）；
 *   3) 离线取证              —— 缓冲区符号 g_klog_buf 固定于内核 .bss，
 *                               GDB/QEMU monitor 直接读内存即可。
 *
 * 无需显式 init：静态存储 + 原子游标，首字符（serial_init 之前）即可记录。
 */
#ifndef _SUKI_KERNEL_KLOG_H
#define _SUKI_KERNEL_KLOG_H

#include <kernel/types.h>

#define KLOG_BUF_SIZE  65536U          /* 必须为 2 的幂（取模用位与） */

/* 记录一个字符（serial_write 内部调用；多核安全：原子游标）。 */
void klog_putc(char c);

/* 环内当前累计写入总字节数（可 > KLOG_BUF_SIZE，表示已回绕）。 */
uint64_t klog_written(void);

/* 把环内现存内容按时间序重放到串口（内部抑制递归记录）。 */
void klog_dump(void);

/* 供 gdbstub qRcmd 遍历：取时间序第 i 个「仍在环内」的字符；
 * i ∈ [0, klog_avail())。 */
uint32_t klog_avail(void);
char klog_at(uint32_t i);

#endif /* _SUKI_KERNEL_KLOG_H */
