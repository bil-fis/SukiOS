/*
 * user/lib/shims/assert.h
 * -----------------------------------------------------------------------------
 * SukiOS 用户态最小 <assert.h>。默认（非 NDEBUG）失败时触发 __builtin_trap()，
 * 不依赖外部 abort() 实现；定义 NDEBUG 时完全展开为空。
 */
#ifndef _SUKI_SHIM_ASSERT_H
#define _SUKI_SHIM_ASSERT_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
#define assert(expr) ((expr) ? (void)0 : (void)__builtin_trap())
#endif

#ifdef __cplusplus
}
#endif

#endif /* _SUKI_SHIM_ASSERT_H */
