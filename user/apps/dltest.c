/*
 * user/apps/dltest.c
 * 动态链接端到端验证程序。
 *
 * 本程序编译期经 -Bdynamic -l:libtest.sl 记录 DT_NEEDED=libtest.sl，内核在
 * execve 加载期自动加载该库并重定位；其中对 libtest.sl 全局变量 sl_const 的
 * 引用会生成 R_X86_64_COPY 拷贝重定位（验证点①：库初始值 42 被拷入主程序 .bss）。
 *
 * 运行期再经 dlopen 路径验证（验证点②~④）：
 *   - dlopen("/LIB/libtest.sl")：同名库去重，返回加载期句柄（DT_NEEDED 依赖）
 *   - dlopen("/LIB/libtest2.sl")：纯运行期加载（非加载期依赖），dlsym 解析并调用
 *   - dlclose：引用计数归零后，内核真正回收该库的用户页物理页与内核 ELF 副本
 *   - 再次 dlopen("/LIB/libtest2.sl")：槽位复用，证明回收彻底、句柄可再分配
 *
 * 退出码 0 表示全部通过。
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include "lib/suki.h"   /* sys_yield()：FS 未就绪时让出 CPU 重试 dlopen */

/* 该引用触发链接器生成 R_X86_64_COPY 拷贝重定位：加载期由内核把 libtest.sl
 * 中 sl_const 的初始值拷入本程序的 .bss 副本，此后本程序访问的是自己的副本。 */
extern int sl_const;

/* 带 FS 重试的 dlopen（开机早期磁盘可能尚未 mount）。 */
static void *dlopen_retry(const char *path)
{
    void *h = NULL;
    for (int i = 0; i < 400; i++) {
        h = dlopen(path, RTLD_NOW);
        if (h) {
            break;
        }
        sys_yield();
    }
    return h;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("=== dltest: 动态链接 / dlopen / COPY / dlclose 回收 验证 ===\n");

    /* ---- 验证点①：R_X86_64_COPY 加载期拷贝重定位 ---- */
    printf("[COPY] sl_const(主程序副本) = %d  (期望 42)\n", sl_const);
    if (sl_const != 42) {
        printf("[FAIL] COPY 重定位未把库初始值 42 拷入主程序副本\n");
        return 1;
    }

    /* ---- 验证点②：同名库去重（DT_NEEDED 加载期依赖）---- */
    void *h1 = dlopen_retry("/LIB/libtest.sl");
    if (!h1) {
        printf("[FAIL] dlopen(\"/LIB/libtest.sl\") returned NULL\n");
        return 1;
    }
    int (*fp_add)(int, int)     = (int (*)(int, int))dlsym(h1, "sl_add");
    int (*fp_sub)(int, int)     = (int (*)(int, int))dlsym(h1, "sl_sub");
    const char *(*fp_name)(void) = (const char *(*)(void))dlsym(h1, "sl_name");
    int *p_const                = (int *)dlsym(h1, "sl_const");
    if (!fp_add || !fp_sub || !fp_name || !p_const) {
        printf("[FAIL] dlsym 未找到符号 (sl_add=%p sl_sub=%p sl_name=%p sl_const=%p)\n",
               (void *)fp_add, (void *)fp_sub, (void *)fp_name, (void *)p_const);
        return 1;
    }
    printf("[OK] 加载期库: sl_add(2,3)=%d  sl_const(库)=%d  sl_sub(9,4)=%d  sl_name()=%s\n",
           fp_add(2, 3), *p_const, fp_sub(9, 4), fp_name());
    printf("[OK] 复用 sl_add(7,8)=%d\n", fp_add(7, 8));
    /* 该库是加载期依赖（refcount=2：加载期+本次 dlopen），单次 dlclose 不卸载 */
    dlclose(h1);

    /* ---- 验证点③：纯运行期 dlopen + dlclose 真正回收物理页 ---- */
    void *h2 = dlopen_retry("/LIB/libtest2.sl");
    if (!h2) {
        printf("[FAIL] dlopen(\"/LIB/libtest2.sl\") returned NULL\n");
        return 1;
    }
    int (*fp2_add)(int, int) = (int (*)(int, int))dlsym(h2, "sl_add");
    if (!fp2_add) {
        printf("[FAIL] dlsym(libtest2, sl_add) 未找到\n");
        return 1;
    }
    printf("[OK] 运行期库: sl_add(7,8)=%d\n", fp2_add(7, 8));  /* 7+8+42=57 */
    /* 该库仅由本次 dlopen 加载（refcount=1），dlclose 归零应触发内核回收 */
    if (dlclose(h2) != 0) {
        printf("[FAIL] dlclose(libtest2) 返回非 0\n");
        return 1;
    }
    printf("[INFO] dlclose(libtest2) 已调用，内核应打印 reclaim 日志并回收物理页\n");

    /* ---- 验证点④：同一库再次 dlopen，证明槽位已回收可复用 ---- */
    void *h3 = dlopen_retry("/LIB/libtest2.sl");
    if (!h3) {
        printf("[FAIL] 再次 dlopen(\"/LIB/libtest2.sl\") 失败（回收后无法复用）\n");
        return 1;
    }
    int (*fp3_add)(int, int) = (int (*)(int, int))dlsym(h3, "sl_add");
    if (!fp3_add) {
        printf("[FAIL] dlsym(复用, sl_add) 未找到\n");
        return 1;
    }
    printf("[OK] 回收后复用: sl_add(1,2)=%d\n", fp3_add(1, 2));  /* 1+2+42=45 */
    dlclose(h3);

    printf("=== dltest PASS ===\n");
    return 0;
}
