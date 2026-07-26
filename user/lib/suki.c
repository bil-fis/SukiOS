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
    }
    int j = 0;
    while (i > 0) {
        buf[j++] = tmp[--i];
    }
    buf[j] = '\0';
    return buf;
}
