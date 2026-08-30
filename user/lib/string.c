/*
 * user/lib/string.c
 * 标准 C 字符串与内存工具（对接 suki.c 底层 memcpy/memset 以保持单一强符号）。
 * 覆盖典型 Linux 程序使用的子集：比较/拷贝/拼接/查找/strtok/strtol/ctype。
 */
#include "libc.h"

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)(*a) - (unsigned char)(*b);
}

int strncmp(const char *a, const char *b, size_t n)
{
    if (!n) return 0;
    while (n-- && *a && (*a == *b)) { a++; b++; }
    if (n == (size_t)-1) return 0;
    return (unsigned char)(*a) - (unsigned char)(*b);
}

char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++) != '\0') { }
    return r;
}

char *strncpy(char *d, const char *s, size_t n)
{
    char *r = d;
    while (n && (*d++ = *s++)) n--;
    while (n--) *d++ = '\0';
    return r;
}

size_t strlcpy(char *d, const char *s, size_t n)
{
    size_t len = strlen(s);
    if (n) {
        size_t c = (len < n) ? len : n - 1;
        memcpy(d, s, c);
        d[c] = '\0';
    }
    return len;
}

char *strcat(char *d, const char *s)
{
    char *r = d;
    while (*r) r++;
    while ((*r++ = *s++) != '\0') { }
    return d;
}

char *strncat(char *d, const char *s, size_t n)
{
    char *r = d;
    while (*r) r++;
    while (n-- && *s) *r++ = *s++;
    *r = '\0';
    return d;
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (!p) return NULL;
    memcpy(p, s, n);
    return p;
}

char *strndup(const char *s, size_t n)
{
    size_t len = 0;
    while (len < n && s[len]) len++;
    char *p = (char *)malloc(len + 1);
    if (!p) return NULL;
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    return (char *)last;
}

char *strstr(const char *hay, const char *needle)
{
    if (!*needle) return (char *)hay;
    while (*hay) {
        const char *h = hay, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)hay;
        hay++;
    }
    return NULL;
}

char *strtok_r(char *s, const char *sep, char **save)
{
    char *p;
    if (s) *save = s;
    p = *save;
    if (!p) return NULL;
    while (*p && strchr(sep, *p)) p++;
    if (!*p) { *save = NULL; return NULL; }
    char *tok = p;
    while (*p && !strchr(sep, *p)) p++;
    if (*p) { *p = '\0'; *save = p + 1; }
    else    { *save = NULL; }
    return tok;
}

char *strtok(char *s, const char *sep)
{
    static char *save = NULL;
    return strtok_r(s, sep, &save);
}

long strtol(const char *s, char **end, int base)
{
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f' || *s == '\v')
        s++;
    int neg = 0;
    if (*s == '+') s++;
    else if (*s == '-') { neg = 1; s++; }
    if (base == 0) {
        if (*s == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (*s == '0') { base = 8; s++; }
        else base = 10;
    }
    long v = 0;
    while (*s) {
        int d;
        char c = *s;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        s++;
    }
    if (end) *end = (char *)s;
    return neg ? -v : v;
}

long long strtoll(const char *s, char **end, int base)
{
    return (long long)strtol(s, end, base);
}

unsigned long strtoul(const char *s, char **end, int base)
{
    return (unsigned long)strtol(s, end, base);
}

int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }

int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isprint(int c) { return c >= 0x20 && c <= 0x7e; }
int toupper(int c) { return islower(c) ? c - 'a' + 'A' : c; }
int tolower(int c) { return isupper(c) ? c - 'a' + 'A' : c; }
