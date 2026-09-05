/*
 * user/lib/freetype_shim.c
 * -----------------------------------------------------------------------------
 * FreeType 标准库适配桩实现。
 *
 * SukiOS 仅以 FT_OPEN_MEMORY 方式加载字体，永不调用这些 stdio 占位函数；
 * 但它们被 ftsystem.c 直接或间接引用，需提供可链接的实现（一律返回失败值）。
 */
#include "user/lib/freetype_shim.h"

int ft_fclose_stub(FILE *f)
{
    (void)f;
    return -1;
}

FILE *ft_fopen_stub(const char *p, const char *m)
{
    (void)p; (void)m;
    return (FILE*)0;   /* 不支持文件流加载 */
}

size_t ft_fread_stub(void *b, size_t s, size_t n, FILE *f)
{
    (void)b; (void)s; (void)n; (void)f;
    return 0;
}

int ft_fseek_stub(FILE *f, long o, int w)
{
    (void)f; (void)o; (void)w;
    return -1;
}

long ft_ftell_stub(FILE *f)
{
    (void)f;
    return -1;
}

/* zlib 桩：本仓仅用 FT_OPEN_MEMORY 加载未压缩 TTF，不触发 gzip 解压路径
 *（woff/svg 等压缩字体不被本服务使用）。提供符号以满足链接，调用则返回错误。 */
int FT_Gzip_Uncompress(void *stream, unsigned char *output,
                      unsigned long *out_len, const unsigned char *input,
                      unsigned long in_len)
{
    (void)stream; (void)output; (void)out_len; (void)input; (void)in_len;
    return 1;   /* 非 0 = 失败 */
}
