/* ishell - SukiOS 用户态 Shell (静态缓冲区版，修复命令解析) */

#define SYSCALL_WRITE 1
#define SYSCALL_READ  2
#define SYSCALL_EXIT  60

/* 使用静态存储，避免栈破坏 */
static char buf[256];
static int idx;

/* 系统调用封装 */
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

static void putchar(char c) {
    syscall(SYSCALL_WRITE, c, 0, 0);
}

static unsigned char getchar(void) {
    long ret = syscall(SYSCALL_READ, 0, 0, 0);
    return (unsigned char)ret;
}

void _start(void) {
    print("SukiOS Shell v0.4 (static buf)\n");

    while (1) {
        idx = 0;
        putchar('>');
        putchar(' ');

        /* 读取一行，不依赖 \0 终止 */
        while (1) {
            unsigned char c = getchar();

            if (c == '\n' || c == '\r') {
                putchar('\n');
                break;
            } else if (c == '\b') {       // 退格键
                if (idx > 0) {
                    idx--;
                    putchar('\b');         // 依赖内核 VGA 驱动的退格处理
                }
            } else {                       // 普通可打印字符
                if (idx < 255) {
                    buf[idx++] = (char)c;
                    putchar((char)c);
                }
            }
        }

        if (idx == 0) continue;

        /* 命令解析：直接比较字符，不使用字符串函数 */
        if (idx == 4) {
            if (buf[0] == 'h' && buf[1] == 'e' && buf[2] == 'l' && buf[3] == 'p') {
                print("Available commands: help, exit\n");
                continue;
            }
            if (buf[0] == 'e' && buf[1] == 'x' && buf[2] == 'i' && buf[3] == 't') {
                print("Bye!\n");
                syscall(SYSCALL_EXIT, 0, 0, 0);
            }
        }

        /* 未知命令：安全输出 idx 个字符 */
        print("Unknown command: ");
        for (int i = 0; i < idx; i++) {
            putchar(buf[i]);
        }
        putchar('\n');
    }
}