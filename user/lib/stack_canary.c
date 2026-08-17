/*
 * user/lib/stack_canary.c
 * -----------------------------------------------------------------------------
 * 用户态栈金丝雀（L5 修复）：为 -fstack-protector-strong 提供 __stack_chk_guard
 * 与 __stack_chk_fail。本文件以 -fno-stack-protector 编译（见 Makefile），
 * 理由同内核（避免 __stack_chk_fail 自身递归插桩）。
 *
 * 调用关系：用户程序被插桩函数返回前比对 canary -> 不匹配 ->
 *           __stack_chk_fail -> 经 syscall 打印告警并 SYS_TASK_EXIT(1)。
 */
#include "suki.h"

/* 固定哨兵（用户态常量，同内核理由）。 */
uintptr_t __stack_chk_guard = 0x005C0FFEE10BADULL;

__attribute__((noreturn))
void __stack_chk_fail(void)
{
    sys_debug_write("user: stack smashing detected\n",
                    sizeof("user: stack smashing detected\n") - 1);
    sys_exit(1);
}
