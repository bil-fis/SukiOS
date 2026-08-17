/*
 * include/kernel/gdbstub.h
 * -----------------------------------------------------------------------------
 * P0-R3c：常备串口 GDB stub（GDB Remote Serial Protocol，COM2 0x2F8）。
 *
 * 能力（RSP 最小集 + 实用扩展）：
 *   ? g G m M c s          —— 停机原因/读写寄存器/读写内存/继续/单步
 *   p P H qSupported qAttached qRcmd(monitor klog) D k
 *   软件断点：GDB 侧经 M 写 0xCC（stub 对 Z0/z0 回空促使 GDB 回退软断点），
 *   断点/单步经 #BP(vec3)/#DB(vec1) 陷入本 stub。
 *
 * 接入方式（QEMU 侧把 COM2 暴露为 TCP 服务端）：
 *   qemu ... -serial file:com1.log -serial tcp::5556,server,nowait
 *   gdb build/kernel.elf
 *     (gdb) target remote localhost:5556     # 任意时刻可附着：
 *   sched_tick(BSP, 100Hz) 轮询 COM2 RX，一旦有数据即触发 int3 陷入会话，
 *   GDB 的首包（qSupported）由会话循环应答，握手自然完成。
 *
 * SMP：会话期间经 IPI_GDB_FREEZE 冻结其它 CPU（自旋），continue/step/detach
 * 时释放，保证内存/寄存器快照一致。
 *
 * KASLR 配合：附着后在 GDB 内执行
 *   (gdb) symbol-file build/kernel.elf -o <slide>
 * slide = 运行期基址 - 0xFFFF800000000000（启动日志 [kaslr] 行有打印）。
 */
#ifndef _SUKI_KERNEL_GDBSTUB_H
#define _SUKI_KERNEL_GDBSTUB_H

#include <kernel/types.h>
#include <kernel/interrupts.h>

/* 初始化 COM2 + 注册 #DB(vec1)/#BP(vec3) 处理器 + 注册冻结 IPI。
 * 在 idt_init 之后、smp_init 之前调用。 */
void gdbstub_init(void);

/* BSP sched_tick 每 10ms 调用：COM2 有数据且未在会话中 -> int3 陷入。
 * 非 BSP / 已附着 / 未初始化时为空操作，开销一次 inb。 */
void gdbstub_poll(void);

#endif /* _SUKI_KERNEL_GDBSTUB_H */
