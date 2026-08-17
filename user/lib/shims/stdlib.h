/*
 * user/lib/shims/stdlib.h
 * -----------------------------------------------------------------------------
 * 最小 stdlib.h 垫片，仅供独立 app（启用 -ffreestanding -nostdlib）编译
 * minimp3 时使用。minimp3.h 在实现块无条件 #include <stdlib.h>，但其基础
 * 解码 API（mp3dec_init / mp3dec_decode_frame）并不真正调用 malloc/free，
 * 故这里留空即可（仅避免“头文件找不到”的致命错误）。
 */
#ifndef _SUKI_SHIM_STDLIB_H
#define _SUKI_SHIM_STDLIB_H
#endif /* _SUKI_SHIM_STDLIB_H */
