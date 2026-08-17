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
    u_print(u_utoa_s(v, buf, sizeof(buf)));
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

    /* ---- P0-5 端到端验证：Ring3 mmap 匿名映射 + 按需分页 ----
     * sys_mmap 只登记 VMA 不给物理页；下面第一次写 buf[i] 触发真实 #PF，
     * 由内核 page_fault_handler -> vma_populate 补零页后 iretq 原地重试。
     * 若按需分页链路损坏，本进程会被杀（打印 killing task），测试即失败。 */
    {
        uint64_t len = 64 * 1024;                        /* 16 页 */
        volatile uint8_t *buf = (volatile uint8_t *)
            sys_mmap(len, SUKI_PROT_WRITE);
        if (!buf) {
            u_print("mmap test: FAIL (sys_mmap returned NULL)\n");
        } else {
            int ok = 1;
            for (uint64_t i = 0; i < len; i += 4096) {
                if (buf[i] != 0) ok = 0;                 /* 应为零页 */
                buf[i] = (uint8_t)(i >> 12) + 1;         /* 写触发 #PF 补页 */
            }
            for (uint64_t i = 0; i < len; i += 4096) {
                if (buf[i] != (uint8_t)((i >> 12) + 1)) ok = 0;
            }
            if (sys_munmap((void *)buf, len) != 0) ok = 0;
            u_print(ok ? "mmap demand-paging test: PASS\n"
                       : "mmap demand-paging test: FAIL\n");
        }
    }

    u_print("hello from a freshly spawned ELF process; exiting now.\n");
    sys_exit(0);
    return 0;   /* 不可达 */
}
