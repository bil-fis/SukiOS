/*
 * user/lib/freetype_shim.h
 * -----------------------------------------------------------------------------
 * FreeType 标准库适配头（替代 ftstdlib.h）。
 *
 * 用法：编译 FreeType 源码时传入
 *   -DFT_CONFIG_STANDARD_LIBRARY_H="\"user/lib/freetype_shim.h\""
 * 这样 freetype 的 config/ftstdlib.h 在检测到 FTSTDLIB_H_ 已定义后整体跳过，
 * 改由本头提供所有 ft_* 宏与整数极限常量。
 *
 * 设计目标（SukiOS freestanding 用户态）：
 *   * 内存管理：复用本仓 libc 的 malloc/free/calloc/realloc（shims/stdlib.h）。
 *   * 字符/串：复用 shims/string.h 的 memstr*。
 *   * 执行控制：setjmp/longjmp 用本仓 user/lib/setjmp.S 实现（shims/setjmp.h）。
 *   * 文件 I/O：FreeType 在本仓**仅以 FT_OPEN_MEMORY 方式加载字体**，
 *     完全不调用 ft_fopen 等 stdio 接口；但 ftsystem.c 仍会编译 ft_fopen 等
 *     符号，故这里仅声明占位原型（实现在 freetype_shim.c，返回失败值），
 *     确保可链接且不真正依赖宿主 stdio。
 *   * 不引入 <stdio.h> 的 FILE 全功能，仅用占位 FILE 类型满足 ftstdlib 的
 *     FT_FILE 宏。
 */
#ifndef FTSTDLIB_H_
#define FTSTDLIB_H_

#include <stddef.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>    /* shims/stdio.h：snprintf/printf；无 FILE 流 */
#include <setjmp.h>   /* shims/setjmp.h：jmp_buf/setjmp/longjmp */

#define ft_ptrdiff_t  ptrdiff_t

/* ---- 整数极限常量 ---- */
#define FT_CHAR_BIT    CHAR_BIT
#define FT_USHORT_MAX  USHRT_MAX
#define FT_INT_MAX     INT_MAX
#define FT_INT_MIN     INT_MIN
#define FT_UINT_MAX    UINT_MAX
#define FT_LONG_MIN    LONG_MIN
#define FT_LONG_MAX    LONG_MAX
#define FT_ULONG_MAX   ULONG_MAX
#ifdef LLONG_MAX
#define FT_LLONG_MAX   LLONG_MAX
#endif
#ifdef LLONG_MIN
#define FT_LLONG_MIN   LLONG_MIN
#endif
#ifdef ULLONG_MAX
#define FT_ULLONG_MAX  ULLONG_MAX
#endif

/* ---- 字符/串处理 ---- */
#define ft_memchr   memchr
#define ft_memcmp   memcmp
#define ft_memcpy   memcpy
#define ft_memmove  memmove
#define ft_memset   memset
#define ft_strcat   strcat
#define ft_strcmp   strcmp
#define ft_strcpy   strcpy
#define ft_strlen   strlen
#define ft_strncmp  strncmp
#define ft_strncpy  strncpy
#define ft_strrchr  strrchr
#define ft_strstr   strstr

/* ---- 文件处理（占位；本仓仅用内存流 FT_OPEN_MEMORY，不真正调用文件 API） ----
 * 直接复用 SukiOS 标准 FILE（user/lib/shims/stdio.h + fileio.c），
 * 避免与标准 <stdio.h> 中的 FILE 定义冲突。 */
#include <stdio.h>
#define FT_FILE      FILE

/* ftsystem.c 编译需要的标准 seek  whence 常量（值等同 POSIX） */
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
/* 这些函数原型在 freetype_shim.c 中提供桩实现（返回失败值） */
int   ft_fclose_stub(FILE *f);
FILE *ft_fopen_stub(const char *p, const char *m);
size_t ft_fread_stub(void *b, size_t s, size_t n, FILE *f);
int   ft_fseek_stub(FILE *f, long o, int w);
long  ft_ftell_stub(FILE *f);
#define ft_fclose    ft_fclose_stub
#define ft_fopen     ft_fopen_stub
#define ft_fread     ft_fread_stub
#define ft_fseek     ft_fseek_stub
#define ft_ftell     ft_ftell_stub
#define ft_snprintf  snprintf

/* ---- 排序 ---- */
#define ft_qsort  qsort

/* ---- 内存分配（复用 libc） ---- */
#define ft_scalloc   calloc
#define ft_sfree     free
#define ft_smalloc   malloc
#define ft_srealloc  realloc

/* ---- 杂项 ---- */
#define ft_strtol  strtol
#define ft_getenv  getenv

/* ---- 执行控制 ---- */
#define ft_jmp_buf     jmp_buf
#define ft_longjmp     longjmp
#define ft_setjmp( b ) setjmp( *(ft_jmp_buf*) &(b) )

#endif /* FTSTDLIB_H_ */
