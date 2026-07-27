/*
 * kernel/stack_canary.c
 * -----------------------------------------------------------------------------
 * 栈金丝雀（L5 修复）：为 -fstack-protector-strong 提供 __stack_chk_guard 与
 * __stack_chk_fail 两个符号。本文件刻意以 -fno-stack-protector 编译（见
 * Makefile）：否则 __stack_chk_fail 自身被插桩后会递归调用自己，且 guard
 * 尚未就绪时插桩函数会误报栈破坏。
 *
 * 调用关系：全内核被插桩的函数在返回前比对 [rsp+X] 处的 canary 与
 *           __stack_chk_guard；不匹配即调用 __stack_chk_fail -> panic。
 */
#include <stdint.h>
#include <kernel/console.h>

/* 固定哨兵值。自由环境无 CSPRNG 时退化为常量；足以检测绝大多数"意外栈
 * 溢出破坏相邻栈帧"类缺陷（多任务下常量可被暴力猜测，但作为基础内存安全
 * 防线已阻断误写）。如需更强随机化，可在 kmain 早期用 rdtsc/HPET 重播种。 */
uintptr_t __stack_chk_guard = 0x00BADC0FFEE10BADULL;

__attribute__((noreturn))
void __stack_chk_fail(void)
{
    panic("stack smashing detected (kernel stack canary corrupted)");
}
