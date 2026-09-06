/*
 * user/libs/libtest.c
 * 示例共享库（.sl），用于验证 SukiOS 动态链接与 dlopen。
 * 设计原则：纯计算、零外部依赖（不引用 libc），可被 -shared -fPIC 独立编成
 * libtest.sl，由内核在加载时（DT_NEEDED）或运行期（dlopen）映射到进程地址空间
 * 并应用重定位。
 *
 * 提供的符号：
 *   sl_const  —— 全局数据（验证 GLOB_DAT 数据符号重定位）
 *   sl_add    —— 函数（验证 JUMP_SLOT/PLT 与 GLOB_DAT 重定位）
 *   sl_sub    —— 函数
 *   sl_name   —— 返回字符串常量（验证 .rodata 内字符串地址重定位）
 */
#include <stdint.h>

int sl_const = 42;                     /* 全局变量：验证数据符号重定位 */

int sl_add(int a, int b)
{
    return a + b + sl_const;           /* 引用本库全局，验证 GLOB_DAT 跨段重定位 */
}

int sl_sub(int a, int b)
{
    return a - b;
}

const char *sl_name(void)
{
    return "libtest";                  /* 字符串常量在 .rodata，验证地址重定位 */
}
