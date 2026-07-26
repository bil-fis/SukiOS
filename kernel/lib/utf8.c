/*
 * kernel/lib/utf8.c
 * -----------------------------------------------------------------------------
 * UTF-8 流式解码器实现。字节级喂入，输出 Unicode 码点。
 *
 * 调用关系：控制台 fbcon_putc() 每收到一个字节调用 utf8_feed()，
 *           得到完整码点后再查字形渲染 -> fb_draw_char()。
 */
#include <kernel/utf8.h>

void utf8_init(utf8_state_t *s)
{
    s->cp = 0;
    s->need = 0;
}

int utf8_feed(utf8_state_t *s, uint8_t b, uint32_t *out)
{
    /* ASCII 快速路径：0xxxxxxx */
    if (b < 0x80) {
        *out = b;
        s->need = 0;
        s->cp = 0;
        return 1;
    }

    /* 首字节 */
    if ((b & 0xE0) == 0xC0) {          /* 110xxxxx : 2 字节 */
        if (b < 0xC2) {                /* 0xC0/0xC1 为过短编码，非法 */
            s->need = 0; *out = 0xFFFD; return -1;
        }
        s->cp = b & 0x1F; s->need = 1; return 0;
    }
    if ((b & 0xF0) == 0xE0) {          /* 1110xxxx : 3 字节（含中文 BMP） */
        s->cp = b & 0x0F; s->need = 2; return 0;
    }
    if ((b & 0xF8) == 0xF0) {          /* 11110xxx : 4 字节 */
        s->cp = b & 0x07; s->need = 3; return 0;
    }

    /* 续字节 10xxxxxx */
    if ((b & 0xC0) == 0x80) {
        if (s->need == 0) {            /* 无对应首字节 */
            *out = 0xFFFD; return -1;
        }
        s->cp = (s->cp << 6) | (b & 0x3F);
        if (--s->need == 0) {
            uint32_t cp = s->cp;
            s->cp = 0;
            if (cp > 0x10FFFF) {       /* 超出 Unicode 标量最大值 */
                *out = 0xFFFD; return -1;
            }
            *out = cp;
            return 1;
        }
        return 0;
    }

    /* 其它均为非法首字节 */
    s->need = 0;
    *out = 0xFFFD;
    return -1;
}
