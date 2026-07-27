/*
 * user/lib/suki.c
 * -----------------------------------------------------------------------------
 * SukiOS 用户态运行库实现（最小字符串/输出工具）。
 */
#include "suki.h"

size_t u_strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

int u_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int u_strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}

void *u_memcpy(void *d, const void *s, size_t n)
{
    uint8_t *dd = (uint8_t *)d;
    const uint8_t *ss = (const uint8_t *)s;
    while (n--) {
        *dd++ = *ss++;
    }
    return d;
}

void *u_memset(void *d, int c, size_t n)
{
    uint8_t *dd = (uint8_t *)d;
    while (n--) {
        *dd++ = (uint8_t)c;
    }
    return d;
}

/* ---- 标准 C 内存原语（供 minimp3 等库链接） ----
 * 注意：不要用 __builtin_* 递归实现；直接逐字节循环，freestanding 安全。 */
void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *dd = (uint8_t *)d;
    const uint8_t *ss = (const uint8_t *)s;
    while (n--) {
        *dd++ = *ss++;
    }
    return d;
}

void *memset(void *d, int c, size_t n)
{
    uint8_t *dd = (uint8_t *)d;
    while (n--) {
        *dd++ = (uint8_t)c;
    }
    return d;
}

/* 处理重叠区间：src<dst 时从尾部倒序拷贝 */
void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *dd = (uint8_t *)d;
    const uint8_t *ss = (const uint8_t *)s;
    if (dd == ss || n == 0) {
        return d;
    }
    if (dd < ss) {
        while (n--) {
            *dd++ = *ss++;
        }
    } else {
        dd += n;
        ss += n;
        while (n--) {
            *--dd = *--ss;
        }
    }
    return d;
}

void u_print(const char *s)
{
    sys_debug_write(s, u_strlen(s));
}

void u_printn(const char *s, size_t n)
{
    sys_debug_write(s, n);
}

/*
 * u_utoa_s：长度安全的十进制转换（M17 接口收尾）。
 * - size 为 buf 总容量（含 NUL）。输出最多写 size-1 个字符 + NUL。
 * - size == 0 时不写任何字节，返回 buf（调用方错误但绝不越界）。
 * - 容量不足以容纳完整数字时，输出被截断为高位在前的前缀并置 NUL
 *   （uint64 十进制最长 20 位，size >= 21 即永不截断）。
 */
char *u_utoa_s(uint64_t v, char *buf, size_t size)
{
    if (size == 0) {
        return buf;
    }
    char tmp[21];               /* uint64 十进制最长 20 位 + 冗余 */
    int i = 0;
    if (v == 0) {
        tmp[i++] = '0';
    }
    while (v && i < 20) {
        tmp[i++] = (char)('0' + v % 10);
        v /= 10;
    }
    size_t j = 0;
    while (i > 0 && j + 1 < size) {     /* 恒预留 1 字节 NUL */
        buf[j++] = tmp[--i];
    }
    buf[j] = '\0';
    return buf;
}

/* 兼容包装：旧接口约定调用方缓冲至少 24 字节（现有调用点均为 char[24]）。
 * 新代码一律使用 u_utoa_s。 */
char *u_utoa(uint64_t v, char *buf)
{
    return u_utoa_s(v, buf, 24);
}
