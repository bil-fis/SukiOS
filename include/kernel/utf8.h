/*
 * include/kernel/utf8.h
 * -----------------------------------------------------------------------------
 * 最小 UTF-8 流式解码器（RFC 3629）。
 *
 * 设计：逐字节喂入（适合控制台一个字符一个字符输出的场景），
 * 内部维护续字节计数状态。一次可得到 1 个 Unicode 码点。
 * 非法序列产出 U+FFFD（替换符），由上层决定是否渲染为兜底方框。
 */
#ifndef _SUKI_KERNEL_UTF8_H
#define _SUKI_KERNEL_UTF8_H

#include <kernel/types.h>

typedef struct {
    uint32_t cp;    /* 已累积的码点 */
    int      need;  /* 还需的续字节数 */
} utf8_state_t;

/* 复位解码器状态（在控制台初始化时调用一次） */
void utf8_init(utf8_state_t *s);

/*
 * 喂入一个字节。
 * 返回：
 *   1  -> 得到完整码点，*out 有效（含 ASCII 单字节）
 *   0  -> 续字节，需要更多字节才能成码点（*out 无效）
 *  -1  -> 非法序列，*out = 0xFFFD（替换符）
 */
int utf8_feed(utf8_state_t *s, uint8_t b, uint32_t *out);

#endif /* _SUKI_KERNEL_UTF8_H */
