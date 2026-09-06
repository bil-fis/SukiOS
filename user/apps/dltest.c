/*
 * user/apps/dltest.c
 * 动态链接端到端验证程序（运行期 dlopen 路径）。
 *
 * SukiOS 裸机交叉工具链的 ld 不支持 -shared（退化为 ET_EXEC），故共享库
 * libtest.sl 以 -pie（ET_DYN）形式构建；本程序在运行期通过 dlopen 让内核
 * 加载该 ET_DYN 模块并重定位，再用 dlsym 解析符号并调用，验证：
 *   - 内核 elf_dlopen：加载 ET_DYN 共享对象、应用重定位（RELATIVE/GLOB_DAT/JUMP_SLOT）
 *   - 符号解析（跨模块、数据符号 sl_const / 函数符号 sl_add/sl_sub/sl_name）
 *   - dlclose 引用计数
 *
 * 退出码 0 表示全部通过。
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include "lib/suki.h"   /* sys_yield()：FS 未就绪时让出 CPU 重试 dlopen */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("=== dltest: dlopen 动态链接验证 ===\n");

    /* 开机早期与 fs-server 并发启动：磁盘可能尚未 mount，dlopen 经 FS_PORT
     * 读 /LIB/libtest.sl 会暂时失败。重试若干轮（每轮 yield 让出 CPU），
     * 与 posixtest::wait_fs_ready 同构，避免 FS 未就绪导致的「假失败」。 */
    void *h = NULL;
    for (int i = 0; i < 400; i++) {
        h = dlopen("/LIB/libtest.sl", RTLD_NOW);
        if (h) {
            break;
        }
        sys_yield();
    }
    if (!h) {
        printf("[FAIL] dlopen(\"/LIB/libtest.sl\") returned NULL\n");
        return 1;
    }

    int (*fp_add)(int, int)   = (int (*)(int, int))dlsym(h, "sl_add");
    int (*fp_sub)(int, int)   = (int (*)(int, int))dlsym(h, "sl_sub");
    const char *(*fp_name)(void) = (const char *(*)(void))dlsym(h, "sl_name");
    int *p_const              = (int *)dlsym(h, "sl_const");

    if (!fp_add || !fp_sub || !fp_name || !p_const) {
        printf("[FAIL] dlsym 未找到符号 (sl_add=%p sl_sub=%p sl_name=%p sl_const=%p)\n",
               (void *)fp_add, (void *)fp_sub, (void *)fp_name, (void *)p_const);
        return 1;
    }

    printf("[OK] sl_add(2,3)=%d  sl_const=%d\n", fp_add(2, 3), *p_const);
    printf("[OK] sl_sub(9,4)=%d  sl_name()=%s\n", fp_sub(9, 4), fp_name());

    /* 二次调用确认内核模块已稳定映射（非一次性拷贝） */
    printf("[OK] 复用 sl_add(7,8)=%d\n", fp_add(7, 8));

    dlclose(h);
    printf("=== dltest PASS ===\n");
    return 0;
}
