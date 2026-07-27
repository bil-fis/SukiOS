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

/* P0-8：用 TSC 熵重播种金丝雀，消除「常量可静态预测」缺口。
 * 安全前提（调用方必须满足，见 kmain）：
 *   1) 在 kmain 极早期、开中断/启动 AP/创建任务之前调用——改写 guard
 *      瞬间不允许任何「序言已读旧值、尾声将比对新值」的插桩函数在飞；
 *   2) kmain 自身虽被插桩但永不返回，其栈帧比对永不发生；
 *   3) 本文件以 -fno-stack-protector 编译，本函数自身无插桩，天然安全。
 * 低字节强制置 0：经典 NUL 终结子设计，阻断 strcpy 类连续字符串溢出
 * 恰好覆盖到 canary 仍保持合法值的利用路径。 */
void stack_canary_reseed(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t x = ((uint64_t)hi << 32) | lo;
    x ^= x >> 33; x *= 0xFF51AFD7ED558CCDUL; x ^= x >> 33;
    x *= 0xC4CEB9FE1A85EC53UL; x ^= x >> 33;
    __stack_chk_guard = x & ~0xFFULL;      /* 低字节 = 0x00（NUL 终结子） */
}
