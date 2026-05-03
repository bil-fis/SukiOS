/* ishell - SukiOS 用户态 Shell */

#define SYSCALL_WRITE 1
#define SYSCALL_READ  2
#define SYSCALL_EXIT  60

/* 内联汇编实现系统调用 */
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

/* 输出字符串 */
static void print(const char *s) {
    while (*s) {
        syscall(SYSCALL_WRITE, *s, 0, 0);
        s++;
    }
}

/* 输出单个字符 */
static void putchar(char c) {
    syscall(SYSCALL_WRITE, c, 0, 0);
}

/* 从键盘读取一个字符（非阻塞，循环等待） */
static char getchar(void) {
    long ret = syscall(SYSCALL_READ, 0, 0, 0);
    return (char)ret;
}

/* 主入口 */
void _start(void) {
    char buf[256];
    int idx;

    print("SukiOS Shell v0.1\n");

    while (1) {
        idx = 0;
        putchar('>');
        putchar(' ');

        /* 读取一行 */
        while (1) {
            char c = getchar();
            if (c == '\n' || c == '\r') {
                buf[idx] = '\0';
                putchar('\n');
                break;
            }
            /* 回显 */
            putchar(c);
            if (idx < 255) {
                buf[idx++] = c;
            }
        }

        /* 命令解析 */
        if (idx == 0) {
            continue;       /* 空行 */
        }

        /* 比较命令（简单字符串比较，没有标准库函数） */
        int is_help = 1;
        const char *help_cmd = "help";
        for (int i = 0; i < 4; i++) {
            if (buf[i] != help_cmd[i]) { is_help = 0; break; }
        }
        if (is_help && buf[4] == '\0') {
            print("Available commands: help, exit\n");
            continue;
        }

        int is_exit = 1;
        const char *exit_cmd = "exit";
        for (int i = 0; i < 4; i++) {
            if (buf[i] != exit_cmd[i]) { is_exit = 0; break; }
        }
        if (is_exit && buf[4] == '\0') {
            print("Bye!\n");
            syscall(SYSCALL_EXIT, 0, 0, 0);
        }

        print("Unknown command: ");
        print(buf);
        putchar('\n');
    }
}