/*
 * user/lib/stdio.c
 * 格式化输出（对接内核 SYS_WRITE 到 fd 1/2）。支持 %d %u %x %X %s %c %p
 * %ld %lu %lx %lld %llu %llx 以及字段宽度（右对齐，如 %5d）。头文件后跟
 * 长度修饰 i/l/ll。供 printf/fprintf/snprintf 共用一个 vsnprintf 内核。
 */
#include "libc.h"

/* 整型转十进制/十六进制，写入 buf（不含 NUL），返回写入长度 */
static int fmt_utoa(char *buf, uint64_t v, int base, int upper)
{
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = dig[v % base]; v /= base; }
    int n = i;
    while (i) *buf++ = tmp[--i];
    return n;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    char *p = buf;
    char *end = buf + (size ? size - 1 : 0);
    int width;
    char c;

    while ((c = *fmt++)) {
        if (c != '%') {
            if (size && p < end) *p = c;
            if (p >= end) p = end; else p++;
            continue;
        }
        /* 解析宽度 */
        width = 0;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        /* 长度修饰 */
        int longness = 0; /* 0=int, 1=long, 2=long long */
        while (*fmt == 'l') { longness++; fmt++; }
        if (*fmt == 'h') { fmt++; longness = 0; }

        c = *fmt++;
        if (c == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            size_t sl = strlen(s);
            if (width > (int)sl) { width -= (int)sl; while (width-- > 0) { if (size && p < end) *p++ = ' '; } }
            while (*s) { if (size && p < end) *p++ = *s; s++; }
        } else if (c == 'c') {
            char cc = (char)va_arg(ap, int);
            if (size && p < end) *p = cc; p++;
        } else if (c == 'd' || c == 'i') {
            int64_t v;
            if (longness >= 2) v = va_arg(ap, long long);
            else if (longness == 1) v = va_arg(ap, long);
            else v = va_arg(ap, int);
            char tmp[24]; int ti = 0; int neg = (v < 0);
            uint64_t uv = neg ? (uint64_t)(-v) : (uint64_t)v;
            if (uv == 0) tmp[ti++] = '0';
            while (uv) { tmp[ti++] = '0' + (uv % 10); uv /= 10; }
            int len = ti + (neg ? 1 : 0);
            if (width > len) { width -= len; while (width-- > 0) { if (size && p < end) *p++ = ' '; } }
            if (neg) { if (size && p < end) *p++ = '-'; }
            while (ti) { if (size && p < end) *p++ = tmp[--ti]; }
        } else if (c == 'u') {
            uint64_t v;
            if (longness >= 2) v = va_arg(ap, unsigned long long);
            else if (longness == 1) v = va_arg(ap, unsigned long);
            else v = va_arg(ap, unsigned int);
            char tmp[24]; int n = fmt_utoa(tmp, v, 10, 0);
            if (width > n) { width -= n; while (width-- > 0) { if (size && p < end) *p++ = ' '; } }
            for (int i = 0; i < n; i++) { if (size && p < end) *p++ = tmp[i]; }
        } else if (c == 'x' || c == 'X') {
            uint64_t v;
            if (longness >= 2) v = va_arg(ap, unsigned long long);
            else if (longness == 1) v = va_arg(ap, unsigned long);
            else v = va_arg(ap, unsigned int);
            char tmp[24]; int n = fmt_utoa(tmp, v, 16, c == 'X');
            if (width > n) { width -= n; while (width-- > 0) { if (size && p < end) *p++ = ' '; } }
            for (int i = 0; i < n; i++) { if (size && p < end) *p++ = tmp[i]; }
        } else if (c == 'p') {
            uint64_t v = (uint64_t)va_arg(ap, void *);
            char tmp[20]; int n = fmt_utoa(tmp, v, 16, 0);
            if (size && p < end) *p++ = '0'; if (size && p < end) *p++ = 'x';
            for (int i = 0; i < n; i++) { if (size && p < end) *p++ = tmp[i]; }
        } else if (c == '%') {
            if (size && p < end) *p++ = '%';
        } else {
            if (size && p < end) *p++ = c;
        }
    }
    if (size) { if (p < end) *p = '\0'; else buf[size - 1] = '\0'; }
    return (int)(p - buf);
}

int printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    suki_syscall3(SYS_WRITE, STDOUT_FILENO, (uint64_t)buf, (uint64_t)n);
    return n;
}

int fprintf(int fd, const char *fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    suki_syscall3(SYS_WRITE, (uint64_t)fd, (uint64_t)buf, (uint64_t)n);
    return n;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int puts(const char *s)
{
    printf("%s\n", s);
    return 0;
}

int putchar(int c)
{
    char ch = (char)c;
    suki_syscall3(SYS_WRITE, STDOUT_FILENO, (uint64_t)&ch, 1);
    return c;
}
