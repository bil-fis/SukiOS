/*
 * include/kernel/font.h
 * -----------------------------------------------------------------------------
 * 字体抽象层。
 *
 * 字形查找链（fb_get_glyph 的回退顺序）：
 *   1) ASCII (0x20-0x7F)   —— 公有领域 8x8 点阵字体 font8x8_basic（放大显示）
 *   2) CJK（中文等）       —— kernel/arch/x86_64/font_cjk.c 的 16x16 点阵子集
 *   3) TTF 矢量字体        —— 【预留】见下方 font_ttf_get 注释；将来由磁盘加载的
 *                             .ttf 经 stb_truetype 风格解析器光栅化到字形位图后
 *                             经此钩子接入。当前未实现，命中返回 false 跳过。
 *   4) 未知码点            —— 返回"兜底方框"，保证总能渲染。
 *
 * 点阵约定（与 font8x8 一致）：行主序；每个像素位 bit0(LSB)=最左像素；
 * 每行 stride = (w+7)/8 字节。fb_draw_char() 按 (row,col) 查
 * bit(col & 7) of bits[row*stride + col/8] 判断该像素是否点亮。
 *
 * glyph_t 接口天然支持任意尺寸位图（w/h 任意、stride=(w+7)/8、scale 放大倍数），
 * 因此 TTF 渲染出的可变尺寸字形位图可直接填入 glyph_t 而无需改动 fb_draw_char。
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

/*
 * TTF 矢量字体查找（【预留接口，当前未实现】）。
 * 将来由磁盘加载的 .ttf 经解析器光栅化后查表；现在恒返回 false，
 * 使 fb_get_glyph 回退到兜底方框。签名与 font_cjk_get 一致，
 * 一旦 TTF 子系统就绪即可在此接入而无需改动调用点。
 */
bool font_ttf_get(uint32_t cp, const uint8_t **bits, int *w, int *h);

#endif /* _SUKI_KERNEL_FONT_H */
