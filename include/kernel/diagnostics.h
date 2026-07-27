/*
 * include/kernel/diagnostics.h
 * -----------------------------------------------------------------------------
 * 内核诊断地基（P0-9）：异常/panic 现场转储（寄存器 + 栈回溯）。
 *
 * 对比现状：原 idt.c/page_fault_handler 仅打印零散字段后 panic()/halt，无
 * 统一寄存器帧、无栈回溯、无符号化，事后无法定位崩溃点。本模块提供：
 *   - diag_dump_registers(): 完整 GPR/段/标志/CR2/CR3 帧；
 *   - diag_dump_trace(): 基于 RBP 链的内核栈回溯（打印返回地址，可由 objdump
 *     离线映射函数，后续可接符号表）；
 *   - kernel_oops(): 异常处理器统一入口（打印+回溯+halt）；
 *   - diag_dump_self(): 供 panic() 在缺少寄存器帧时打印当前栈回溯。
 */
#ifndef _SUKI_KERNEL_DIAGNOSTICS_H
#define _SUKI_KERNEL_DIAGNOSTICS_H

#include <kernel/types.h>
#include <kernel/interrupts.h>

/* 转储完整寄存器帧（含 CR2/CR3） */
void diag_dump_registers(registers_t *r);

/* 栈回溯：从给定 rbp 链向上走，打印每层返回地址。
 * rip 仅用于打印栈顶当前指令（可为 0）。 */
void diag_dump_trace(uint64_t rbp, uint64_t rip);

/* 内核 oops：异常/内核缺页统一入口，打印消息+寄存器+回溯后停机。 */
void kernel_oops(const char *msg, registers_t *r);

/* 供 panic() 调用：打印当前执行流的栈回溯（无寄存器帧时用）。 */
void diag_dump_self(void);

#endif /* _SUKI_KERNEL_DIAGNOSTICS_H */
