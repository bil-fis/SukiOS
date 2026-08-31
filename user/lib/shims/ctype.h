/*
 * user/lib/shims/ctype.h
 * -----------------------------------------------------------------------------
 * SukiOS 用户态标准 <ctype.h> 实现头。
 *
 * 实现归属：user/lib/string.c（isalpha/isdigit/.../toupper/tolower）。
 * 这些函数经 libc.h 已声明，此处作为独立标准头提供，供第三方代码直接
 * #include <ctype.h> 使用。
 */
#ifndef _SUKI_SHIM_CTYPE_H
#define _SUKI_SHIM_CTYPE_H

#ifndef NULL
#define NULL ((void *)0)
#endif

int isalpha(int c);
int isdigit(int c);
int isalnum(int c);
int isspace(int c);
int isupper(int c);
int islower(int c);
int isprint(int c);
int toupper(int c);
int tolower(int c);

#endif /* _SUKI_SHIM_CTYPE_H */
