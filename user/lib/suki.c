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

char *u_utoa(uint64_t v, char *buf)
{
    char tmp[24];
    int i = 0;
    if (v == 0) {
        tmp[i++] = '0';
    }
    while (v) {
        tmp[i++] = (char)('0' + v % 10);
        v /= 10;
        if (i >= 23) break;     /* M17：防御上界，杜绝调用方缓冲不足时越界写 */
    }
    int j = 0;
    while (i > 0) {
        buf[j++] = tmp[--i];
        if (j >= 23) {          /* 同守输出缓冲，至多写 23 字符 + NUL */
            buf[j] = '\0';
            return buf;
        }
    }
    buf[j] = '\0';
    return buf;
}
