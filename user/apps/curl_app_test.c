/*
 * user/apps/curl_app_test.c
 * -----------------------------------------------------------------------------
 * curl 命令行应用的端到端验证（开机自检，由 kmain 内嵌 spawn）。
 *
 * 做法：本程序是一个极小的 execve 包装器——它不等价于内嵌 curl，而是真的
 * 【从 FAT32 磁盘装载 ::BIN/CURL.SKA 并执行】（走 shell 装载同一条路径：
 * execve → 内核读盘 → ELF 加载），从而验证「磁盘上的 curl 应用」可运行。
 *
 * 传参模拟用户在 shell 中的输入：
 *     curl -sS http://10.0.2.2:8000/hello.txt
 * 由 curl CLI 自身完成参数解析、DNS/连接、HTTP 请求并把响应体写到 stdout
 * （串口），可直接在无头回归中判读。
 *
 * 磁盘挂载（AHCI + FS_SERVER）晚于 boot_late_init，故 execve 失败时小睡重试。
 */
#include "lib/suki.h"
#include <string.h>
#include <time.h>

static void pu(uint64_t v)
{
    char b[24];
    u_print(u_utoa_s(v, b, sizeof(b)));
}

int main(void)
{
    /* -v：详细日志写 stderr，可在无头串口日志中判读「CLI 是否真正执行了请求」 */
    static const char *argv[] = {
        "curl", "-v", "-sS", "http://10.0.2.2:8000/hello.txt", NULL
    };
    static const char *envp[] = { NULL };

    u_print("[curl-app] exec /BIN/CURL.SKA: curl -sS http://10.0.2.2:8000/hello.txt\n");

    for (int attempt = 0; attempt < 400; attempt++) {
        long r = (long)suki_syscall3(SYS_EXECVE,
                                     (uint64_t)"/BIN/CURL.SKA",
                                     (uint64_t)argv, (uint64_t)envp);
        if (r >= 0) {
            /* execve 成功不会返回（映像被替换）；此分支理论上不可达 */
            u_print("[curl-app] execve returned unexpectedly\n");
            sys_exit(1);
        }
        if ((attempt % 40) == 0) {
            u_print("[curl-app] execve retry "); pu((uint64_t)attempt);
            u_print(" (rc="); pu((uint64_t)r); u_print(")\n");
        }
        struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 50 * 1000 * 1000;
        nanosleep(&ts, (struct timespec *)0);
    }

    u_print("[curl-app] exec /BIN/CURL.SKA FAIL\n");
    sys_exit(1);
    return 0;
}
