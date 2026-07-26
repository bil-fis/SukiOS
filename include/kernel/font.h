/*
 * include/kernel/font.h
 * -----------------------------------------------------------------------------
 * 字体抽象层。
 *
 * - ASCII (0x20-0x7F) 使用公有领域 8x8 点阵字体 font8x8_basic（放大显示）。
 * - CJK（中文等）使用 kernel/arch/x86_64/font_cjk.c 中的 16x16 点阵子集。
 * - 任意未知码点由 fb_get_glyph() 返回"兜底方框"，保证总能渲染。
 *
 * 点阵约定（与 font8x8 一致）：行主序；每个像素位 bit0(LSB)=最左像素；
 * 每行 stride = (w+7)/8 字节。fb_draw_char() 按 (row,col) 查
 * bit(col & 7) of bits[row*stride + col/8] 判断该像素是否点亮。
 */
#ifndef _SUKI_KERNEL_FONT_H
#define _SUKI_KERNEL_FONT_H

#include <kernel/types.h>

#define FONT_WIDTH   8
#define FONT_HEIGHT  8

extern const uint8_t font8x8_basic[128][8];

/* 字形描述 */
typedef struct {
    const uint8_t *bits;   /* 点阵数据，行主序 */
    int w, h;              /* 像素尺寸 */
    int stride;            /* 每行字节数 = (w+7)/8 */
    int scale;             /* 放大倍数（绘制时每像素 -> scale×scale 方块） */
} glyph_t;

/*
 * 取码点 cp 的字形填充到 g。
 * 返回 true（始终可渲染：ASCII / CJK / 兜底方框）。
 */
bool fb_get_glyph(uint32_t cp, glyph_t *g);

/*
 * CJK 查找：在 16x16 子集表中二分查找码点 cp。
 * 命中则 *bits 指向 32 字节点阵、*w=*h=16，返回 true；否则返回 false。
 */
bool font_cjk_get(uint32_t cp, const uint8_t **bits, int *w, int *h);

#endif /* _SUKI_KERNEL_FONT_H */
