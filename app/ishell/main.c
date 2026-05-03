/* ishell - SukiOS 用户态 Shell (静态缓冲区版，修复命令解析) */

#define SYSCALL_WRITE 1
#define SYSCALL_READ  2
#define SYSCALL_EXECVE 59
#define SYSCALL_EXIT  60
#include <stddef.h>
#include <stdbool.h>

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

/* execve 系统调用封装 */
static long sys_execve(const char *path, char **argv, char **envp) {
    return syscall(SYSCALL_EXECVE, (long)path, (long)argv, (long)envp);
}

/* 简单的字符串比较函数（避免使用标准库） */
static int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s2 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return *s1 - *s2;
}

/* 查找字符串中的字符 */
static char *strchr(const char *s, char c) {
    while (*s) {
        if (*s == c) return (char*)s;
        s++;
    }
    return NULL;
}

/* 复制字符串 */
static char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while (*src) {
        *d++ = *src++;
    }
    *d = '\0';
    return dest;
}

/* 计算字符串长度 */
static int strlen(const char *s) {
    int len = 0;
    while (*s++) len++;
    return len;
}

void _start(void) {
    print("SukiOS [v0.0.8.0 iShell v0.4]\n(c) 2026 CaraLiwa. Licensed under GPL-3.0\n");

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

        /* 命令解析和执行 */
        buf[idx] = '\0'; // 添加字符串终止符
        
        // 检查空行
        if (idx == 0) continue;
        
        // 查找第一个空格（命令分隔符）
        char *space = strchr(buf, ' ');
        if (space) {
            *space = '\0'; // 分割命令和参数
        }
        
        // 执行命令
        if (strcmp(buf, "help") == 0) {
            print("Available commands:\n");
            print("  help     - Show the help\n");
            print("  exit     - Exit shell\n");
            print("  ls       - List the file in the root dir\n");
            print("  cat      - Show README.TXT\n");
            print("  echo     - Echo text\n");
            print("  exec     - Run application\n");
            print("  [cmd]    - Run application\n");
            continue;
        }
        
        if (strcmp(buf, "exit") == 0) {
            print("Bye!\n");
            syscall(SYSCALL_EXIT, 0, 0, 0);
        }
        
        if (strcmp(buf, "ls") == 0) {
            // 简单的ls实现：列出根目录
            print("/ISHELL\n/README.TXT\n");
            continue;
        }
        
        if (strcmp(buf, "cat") == 0) {
            // 简单的cat实现：显示README.TXT
            print("--- README.TXT ---\n");
            print("SukiOS is a from-scratch x86_64 monolithic kernel.\n");
            print("--- end ---\n");
            continue;
        }
        
        if (strcmp(buf, "echo") == 0) {
            // echo 命令实现
            bool newline = true;
            char *arg_start = NULL;
            
            // 检查是否有 -n 选项
            if (space) {
                char *next_space = strchr(space + 1, ' ');
                if (next_space) {
                    *next_space = '\0';
                    if (strcmp(space + 1, "-n") == 0) {
                        newline = false;
                        arg_start = next_space + 1;
                        // 恢复空格
                        *next_space = ' ';
                    } else {
                        arg_start = space + 1;
                    }
                } else {
                    arg_start = space + 1;
                }
                
                // 跳过前导空格
                while (arg_start && *arg_start == ' ') {
                    arg_start++;
                }
                
                // 输出参数（支持简单引号）
                if (arg_start && *arg_start) {
                    char *p = arg_start;
                    while (*p) {
                        // 处理双引号
                        if (*p == '"') {
                            p++; // 跳过引号
                            while (*p && *p != '"') {
                                putchar(*p);
                                p++;
                            }
                            if (*p == '"') p++; // 跳过结束引号
                            if (*p == ' ') p++; // 跳过空格
                        } else if (*p == ' ' && p > arg_start && *(p-1) != ' ') {
                            // 多个空格只输出一个
                            putchar(' ');
                            while (*p == ' ') p++;
                            continue;
                        } else if (*p != ' ') {
                            putchar(*p);
                            p++;
                        } else {
                            p++;
                        }
                    }
                } else {
                    // 没有参数时输出空行（如果需要换行）
                    if (newline) {
                        putchar('\n');
                    }
                }
            }
            
            if (newline && !arg_start) {
                // 没有参数且需要换行
                putchar('\n');
            }
            continue;
        }
        
        if (strcmp(buf, "exec") == 0) {
            // exec命令：执行指定程序
            if (space) {
                char *program = space + 1;
                while (*program == ' ') program++; // 跳过前导空格
                
                if (*program) {
                    // 解析参数（简单实现）
                    char *args[8]; // 最多8个参数
                    int arg_count = 0;
                    
                    // 第一个参数是程序名
                    args[arg_count++] = program;
                    
                    // 查找后续空格分隔的参数
                    char *p = program;
                    while (*p && arg_count < 7) {
                        if (*p == ' ') {
                            *p = '\0'; // 分割参数
                            p++;
                            while (*p == ' ') p++; // 跳过空格
                            if (*p) {
                                args[arg_count++] = p;
                            }
                        } else {
                            p++;
                        }
                    }
                    args[arg_count] = NULL; // argv必须以NULL结尾
                    
                    // 打印调试信息
                    print("exec: trying \"");
                    print(program);
                    print("\"\n");
                    
                    // 尝试执行程序，先尝试带路径
                    long ret = sys_execve(program, args, NULL);
                    if (ret == 0) {
                        // execve成功，当前进程被替换
                        // 不会返回到这里
                    } else {
                        // 尝试添加前缀 / 如果没有
                        if (program[0] != '/') {
                            char full_path[256];
                            full_path[0] = '/';
                            int i;
                            for (i = 0; i < 254 && program[i]; i++) {
                                full_path[i+1] = program[i];
                            }
                            full_path[i+1] = '\0';
                            
                            print("exec: trying \"");
                            print(full_path);
                            print("\"\n");
                            
                            ret = sys_execve(full_path, args, NULL);
                            if (ret == 0) {
                                // execve成功
                            } else {
                                print("exec failed: ");
                                if (ret == -2) print("No such file\n");
                                else if (ret == -12) print("Out of memory\n");
                                else if (ret == -1) print("Invalid ELF format\n");
                                else print("Unknown error\n");
                            }
                        } else {
                            print("exec failed: ");
                            if (ret == -2) print("No such file\n");
                            else if (ret == -12) print("Out of memory\n");
                            else if (ret == -1) print("Invalid ELF format\n");
                            else print("Unknown error\n");
                        }
                    }
                } else {
                    print("exec: no program specified\n");
                }
            } else {
                print("exec: no program specified\n");
            }
            continue;
        }
        
        // 尝试直接执行命令作为程序名
        long ret = sys_execve(buf, NULL, NULL);
        if (ret == 0) {
            // execve成功，当前进程被替换
            // 不会返回到这里
        } else {
            print("Command not found: ");
            for (int i = 0; i < idx; i++) {
                putchar(buf[i]);
            }
            putchar('\n');
        }
    }
}