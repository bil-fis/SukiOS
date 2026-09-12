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
    char c;

    /* 输出一个字符：仅在缓冲未满时写入并推进；满后停止（保持有界） */
#define VSN_PUT(ch) do { if (size && p < end) *p++ = (char)(ch); } while (0)

    while ((c = *fmt++)) {
        if (c != '%') {
            VSN_PUT(c);
            continue;
        }
        /* 解析标志位：'-' 左对齐；'0' 零填充（数值域） */
        int left = 0;
        char pad = ' ';
        for (;;) {
            if (*fmt == '-') { left = 1; fmt++; }
            else if (*fmt == '0') { pad = '0'; fmt++; }
            else break;
        }
        /* 解析宽度（数字或 '*'） */
        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int); fmt++;
            if (width < 0) { left = 1; width = -width; }
        } else {
            while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        }
        /* 解析精度（'.' 后数字或 '*'；-1 表示未指定） */
        int prec = -1;
        if (*fmt == '.') {
            fmt++; prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int); fmt++;
                if (prec < 0) prec = -1;
            } else {
                while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
            }
        }
        /* 长度修饰 */
        int longness = 0; /* 0=int, 1=long, 2=long long */
        while (*fmt == 'l') { longness++; fmt++; }
        if (*fmt == 'h') { fmt++; longness = 0; }

        c = *fmt++;
        if (c == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            size_t sl = strlen(s);
            if (prec >= 0 && (size_t)prec < sl) sl = (size_t)prec;   /* %.Ns 截断 */
            int n = (width > (int)sl) ? width - (int)sl : 0;
            if (!left) while (n-- > 0) VSN_PUT(' ');
            for (size_t i = 0; i < sl; i++) VSN_PUT(s[i]);
            if (left) while (n-- > 0) VSN_PUT(' ');
        } else if (c == 'c') {
            char cc = (char)va_arg(ap, int);
            int n = (width > 1) ? width - 1 : 0;
            if (!left) while (n-- > 0) VSN_PUT(' ');
            VSN_PUT(cc);
            if (left) while (n-- > 0) VSN_PUT(' ');
        } else if (c == 'd' || c == 'i') {
            int64_t v;
            if (longness >= 2) v = va_arg(ap, long long);
            else if (longness == 1) v = va_arg(ap, long);
            else v = va_arg(ap, int);
            char tmp[24]; int ti = 0; int neg = (v < 0);
            /* 取绝对值：先 +1 再取负，避免 INT64_MIN 取负溢出（UB） */
            uint64_t uv = neg ? ((uint64_t)(-(v + 1)) + 1u) : (uint64_t)v;
            if (uv == 0) { if (prec != 0) tmp[ti++] = '0'; }
            while (uv) { tmp[ti++] = (char)('0' + (int)(uv % 10)); uv /= 10; }
            int mindig = (prec > ti) ? prec : ti;          /* 精度：最少位数（补前导 0） */
            int len = (neg ? 1 : 0) + mindig;
            int n = (width > len) ? width - len : 0;
            int zpad = (pad == '0' && prec < 0 && !left);
            if (!left && !zpad) while (n-- > 0) VSN_PUT(' ');
            if (neg) VSN_PUT('-');
            if (zpad) while (n-- > 0) VSN_PUT('0');
            for (int z = ti; z < mindig; z++) VSN_PUT('0');
            while (ti) VSN_PUT(tmp[--ti]);
            if (left) while (n-- > 0) VSN_PUT(' ');
        } else if (c == 'u' || c == 'x' || c == 'X') {
            uint64_t v;
            if (longness >= 2) v = va_arg(ap, unsigned long long);
            else if (longness == 1) v = va_arg(ap, unsigned long);
            else v = va_arg(ap, unsigned int);
            int base = (c == 'u') ? 10 : 16;
            char tmp[24]; int n2 = fmt_utoa(tmp, v, base, c == 'X');
            if (prec == 0 && v == 0) n2 = 0;               /* %.0x 的 0 -> 空 */
            int mindig = (prec > n2) ? prec : n2;
            int n = (width > mindig) ? width - mindig : 0;
            int zpad = (pad == '0' && prec < 0 && !left);
            if (!left && !zpad) while (n-- > 0) VSN_PUT(' ');
            if (zpad) while (n-- > 0) VSN_PUT('0');
            for (int z = n2; z < mindig; z++) VSN_PUT('0');
            for (int i = 0; i < n2; i++) VSN_PUT(tmp[i]);
            if (left) while (n-- > 0) VSN_PUT(' ');
        } else if (c == 'p') {
            uint64_t v = (uint64_t)va_arg(ap, void *);
            char tmp[20]; int n2 = fmt_utoa(tmp, v, 16, 0);
            int len = n2 + 2;
            int n = (width > len) ? width - len : 0;
            if (!left) while (n-- > 0) VSN_PUT(' ');
            VSN_PUT('0'); VSN_PUT('x');
            for (int i = 0; i < n2; i++) VSN_PUT(tmp[i]);
            if (left) while (n-- > 0) VSN_PUT(' ');
        } else if (c == '%') {
            VSN_PUT('%');
        } else {
            VSN_PUT(c);
        }
    }
#undef VSN_PUT
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
