/* user/lib/shims/inttypes.h
 * --------------------------------------------------------------------------
 * 交叉工具链（x86_64-sukios-elf，freestanding）不自带 inttypes.h，而 lwIP 的
 * lwip/arch.h 会 #include <inttypes.h>。此处提供最小实现：包含 stdint.h 并给出
 * lwIP 调试打印占位所需的 PRI* 宏。LWIP_DEBUG=0 时这些宏实际不会被展开，但
 * 头文件必须可被 #include 成功。
 */
#ifndef SUKI_SHIMS_INTTYPES_H
#define SUKI_SHIMS_INTTYPES_H

#include <stdint.h>

#define PRId8   "hhd"
#define PRIi8   "hhi"
#define PRIu8   "hhu"
#define PRIo8   "hho"
#define PRIx8   "hhx"
#define PRIX8   "hhX"

#define PRId16  "hd"
#define PRIi16  "hi"
#define PRIu16  "hu"
#define PRIo16  "ho"
#define PRIx16  "hx"
#define PRIX16  "hX"

#define PRId32  "d"
#define PRIi32  "i"
#define PRIu32  "u"
#define PRIo32  "o"
#define PRIx32  "x"
#define PRIX32  "X"

#define PRId64  "lld"
#define PRIi64  "lli"
#define PRIu64  "llu"
#define PRIo64  "llo"
#define PRIx64  "llx"
#define PRIX64  "llX"

#define PRIdPTR "ld"
#define PRIiPTR "li"
#define PRIuPTR "lu"
#define PRIoPTR "lo"
#define PRIxPTR "lx"
#define PRIXPTR "lX"

#endif /* SUKI_SHIMS_INTTYPES_H */
