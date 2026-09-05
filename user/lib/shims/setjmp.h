/*
 * user/lib/shims/setjmp.h
 * -----------------------------------------------------------------------------
 * SukiOS 用户态 freestanding <setjmp.h> 实现头。
 *
 * 为 FreeType（ftstdlib.h 通过 ft_setjmp/ft_longjmp 使用）与用户程序提供
 * setjmp/longjmp 支持。jmp_buf 布局与 user/lib/setjmp.S 中的保存/恢复次序严格一致：
 *   索引 0..6 保存通用寄存器 RBX,RBP,R12,R13,R14,R15,RSP；索引 7 保存返回地址 RIP。
 * 共 8 个 uint64_t = 64 字节，ABI 对齐足够。
 */
#ifndef _SUKI_SHIM_SETJMP_H
#define _SUKI_SHIM_SETJMP_H

#include <stdint.h>

typedef uint64_t jmp_buf[8];

/* 保存当前执行上下文到 env，返回 0；被 longjmp 恢复时返回非零 val */
int setjmp(jmp_buf env);

/* 恢复 env 保存的上下文，使对应 setjmp 调用返回 val（val==0 时返回 1） */
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

#endif /* _SUKI_SHIM_SETJMP_H */
