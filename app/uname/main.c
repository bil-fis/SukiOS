#define SYSCALL_WRITE 1
#define SYSCALL_READ  2
#define SYSCALL_EXECVE 59
#define SYSCALL_EXIT  60
#include <stddef.h>

static inline long syscall(long num, long arg1, long arg2, long arg3) {
    long ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3)
        : "memory"
    );
    return ret;
}


static void print(const char *s) {
    while (*s) {
        syscall(SYSCALL_WRITE, *s, 0, 0);
        s++;
    }
}

void _start(void)
{
    print("SukiOS 0.0.8.0\n");
}