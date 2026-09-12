/*
 * kernel/abilities/miniz/shim/assert.h
 * -----------------------------------------------------------------------------
 * 内核 freestanding 环境没有 libc <assert.h>；miniz.h 无条件包含它。
 * 此处提供最小实现：断言失败只打印内核告警并继续（不 panic），
 * 与用户态 shim 的语义保持一致的“可诊断但不致命”。
 */
#ifndef _KMINIZ_SHIM_ASSERT_H
#define _KMINIZ_SHIM_ASSERT_H

void __kminiz_assert_fail(const char *expr, const char *file, int line);

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
#define assert(expr) do { if (!(expr)) __kminiz_assert_fail(#expr, __FILE__, __LINE__); } while (0)
#endif

#endif /* _KMINIZ_SHIM_ASSERT_H */
