/*
 * user/apps/hello.c
 * -----------------------------------------------------------------------------
 * 独立程序（standalone app）：由 Shell 经 sys_task_spawn / sys_execve 从
 * FAT32 磁盘 (::BIN/HELLO，无 .elf 后缀) 读取 ELF 字节，由内核 ELF 加载器
 * 装载为新进程映像后执行。
 *
 * 功能：打印成功标记、argc、逐条 argv，然后干净退出，用于验证
 * “ELF 加载器 + spawn/execve 系统调用” 全链路（装载/ASLR 栈/auxv/crt0）。
 */
#include "lib/suki.h"

static void print_dec(uint64_t v)
{
    char buf[24];
    u_print(u_utoa(v, buf));
}

int main(int argc, char **argv)
{
    u_print("=== app HELLO loaded as ELF64 and now running ===\n");
    u_print("argc = ");
    print_dec((uint64_t)argc);
    u_print("\n");

    for (int i = 0; i < argc; i++) {
        u_print("  argv[");
        print_dec((uint64_t)i);
        u_print("] = ");
        u_print(argv[i]);
        u_print("\n");
    }

    u_print("hello from a freshly spawned ELF process; exiting now.\n");
    sys_exit(0);
    return 0;   /* 不可达 */
}
