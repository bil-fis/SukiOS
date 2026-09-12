/*
 * user/lib/fileio.c
 * -----------------------------------------------------------------------------
 * SukiOS 用户态 FILE* 流层（最小实现，面向内核 fd 的薄封装）。
 *
 * 目的：为第三方库（如 libcurl）提供标准 stdio 的 FILE 抽象与符号
 * （stdin/stdout/stderr、fopen/fclose/fread/fwrite/fgets/fprintf/...），
 * 使其默认读写回调（fwrite/fread）与 cookie/文件读写路径可编译、可链接、可运行。
 *
 * 设计：struct _SUKI_FILE 仅持有内核 fd + eof/err 标志；无缓冲（直接 read/write），
 * 因为 SukiOS 用户态内核调用本身即为同步阻塞语义，缓冲意义有限。
 */
#include "libc.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

struct _SUKI_FILE {
    int fd;             /* 内核文件描述符 */
    int eof;            /* 读到 EOF 标志 */
    int err;            /* 错误标志 */
    int close_on_close; /* fclose 时是否 close(fd)（标准流不关） */
};

static struct _SUKI_FILE g_stdin  = { STDIN_FILENO,  0, 0, 0 };
static struct _SUKI_FILE g_stdout = { STDOUT_FILENO, 0, 0, 0 };
static struct _SUKI_FILE g_stderr = { STDERR_FILENO, 0, 0, 0 };

FILE *stdin  = &g_stdin;
FILE *stdout = &g_stdout;
FILE *stderr = &g_stderr;

int fileno(FILE *fp) { return fp ? fp->fd : -1; }

FILE *fopen(const char *path, const char *mode)
{
    if (!path || !mode) { errno = EINVAL; return NULL; }

    int flags = 0;
    int plus = (strchr(mode, '+') != NULL);
    switch (mode[0]) {
    case 'r': flags = plus ? O_RDWR : O_RDONLY; break;
    case 'w': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; break;
    case 'a': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND; break;
    default:  errno = EINVAL; return NULL;
    }

    int fd = open(path, flags, 0644);
    if (fd < 0) return NULL;

    FILE *fp = (FILE *)malloc(sizeof(struct _SUKI_FILE));
    if (!fp) { close(fd); errno = ENOMEM; return NULL; }
    fp->fd = fd;
    fp->eof = 0;
    fp->err = 0;
    fp->close_on_close = 1;
    return fp;
}

FILE *fdopen(int fd, const char *mode)
{
    (void)mode;   /* 无缓冲实现：模式不影响行为 */
    if (fd < 0) { errno = EINVAL; return NULL; }
    FILE *fp = (FILE *)malloc(sizeof(struct _SUKI_FILE));
    if (!fp) { errno = ENOMEM; return NULL; }
    fp->fd = fd;
    fp->eof = 0;
    fp->err = 0;
    fp->close_on_close = 1;
    return fp;
}

int fclose(FILE *fp)
{
    if (!fp) { errno = EINVAL; return EOF; }
    int rc = 0;
    if (fp->close_on_close && fp->fd >= 3)
        rc = close(fp->fd);
    free(fp);
    return rc;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp)
{
    if (!fp || !ptr || size == 0 || nmemb == 0) return 0;
    size_t total = size * nmemb;
    const char *p = (const char *)ptr;
    size_t done = 0;
    while (done < total) {
        ssize_t w = write(fp->fd, p + done, total - done);
        if (w <= 0) { fp->err = 1; break; }
        done += (size_t)w;
    }
    return done / size;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp)
{
    if (!fp || !ptr || size == 0 || nmemb == 0) return 0;
    size_t total = size * nmemb;
    char *p = (char *)ptr;
    size_t done = 0;
    while (done < total) {
        ssize_t r = read(fp->fd, p + done, total - done);
        if (r < 0) { fp->err = 1; break; }
        if (r == 0) { fp->eof = 1; break; }
        done += (size_t)r;
    }
    return done / size;
}

int fputc(int c, FILE *fp)
{
    if (!fp) return EOF;
    unsigned char ch = (unsigned char)c;
    ssize_t w = write(fp->fd, &ch, 1);
    if (w != 1) { fp->err = 1; return EOF; }
    return (int)ch;
}

int fgetc(FILE *fp)
{
    if (!fp) return EOF;
    unsigned char ch;
    ssize_t r = read(fp->fd, &ch, 1);
    if (r <= 0) { if (r == 0) fp->eof = 1; else fp->err = 1; return EOF; }
    return (int)ch;
}

int fputs(const char *s, FILE *fp)
{
    if (!s || !fp) return EOF;
    size_t n = strlen(s);
    return (fwrite(s, 1, n, fp) == n) ? 0 : EOF;
}

char *fgets(char *s, int size, FILE *fp)
{
    if (!s || size <= 0 || !fp) return NULL;
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(fp);
        if (c == EOF) { if (i == 0) return NULL; break; }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = '\0';
    return s;
}

int fflush(FILE *fp) { (void)fp; return 0; }
int ferror(FILE *fp) { return fp ? fp->err : 0; }
int feof(FILE *fp)   { return fp ? fp->eof : 0; }
void clearerr(FILE *fp) { if (fp) { fp->err = 0; fp->eof = 0; } }

int fseek(FILE *fp, long offset, int whence)
{
    if (!fp) return -1;
    if (lseek(fp->fd, (off_t)offset, whence) < 0) { fp->err = 1; return -1; }
    fp->eof = 0;
    return 0;
}

long ftell(FILE *fp)
{
    if (!fp) return -1;
    off_t o = lseek(fp->fd, 0, SEEK_CUR);
    return (long)o;
}

void rewind(FILE *fp) { if (fp) fseek(fp, 0, SEEK_SET); }

int vfprintf(FILE *fp, const char *fmt, va_list ap)
{
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n > 0) {
        size_t len = ((size_t)n < sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;
        fwrite(buf, 1, len, fp);
    }
    return n;
}

int fprintf(FILE *fp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(fp, fmt, ap);
    va_end(ap);
    return n;
}
