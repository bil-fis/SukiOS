/*
 * user/shell.c
 * -----------------------------------------------------------------------------
 * SukiOS Shell —— Ring3 命令行解释器（手册最小可运行系统要求）。
 *
 * 输入：SHELL_PORT 阻塞接收
 *   msgh_id=1 : 键盘字符（INPUT_SERVER 解析后逐字符发来）
 *   msgh_id=FS_MSG_* : FS_SERVER 的应答
 * 输出：sys_debug_write -> 内核统一控制台（帧缓冲图形终端 + 串口）
 *
 * 内置命令：help / ls / cat <FILE> / reboot / uptime? (help 里列出核心 4 个)
 *
 * 调用关系：INPUT_SERVER --IPC--> shell --IPC--> FS_SERVER --IPC--> DISK_PORT。
 */
#include "lib/suki.h"
#include <ipc/fs_proto.h>

#define MSG_ID_KEYCHAR 100   /* 与 input_server 一致；避开 FS_MSG_* */
#define LINE_MAX 120

static uint8_t g_rx[sizeof(mach_msg_header_t) + sizeof(fs_resp_t) + FS_DATA_MAX + 16];

static void prompt(void)
{
    u_print("SukiOS> ");
}

/* 发送 FS 请求（应答异步回到 SHELL_PORT，由主循环等待收取） */
static void fs_request(uint32_t id, const char *arg)
{
    struct {
        mach_msg_header_t h;
        char name[32];
    } req;
    uint32_t payload = 0;
    if (arg) {
        while (arg[payload] && payload < sizeof(req.name) - 1) {
            req.name[payload] = arg[payload];
            payload++;
        }
        req.name[payload++] = '\0';
    }
    req.h.msgh_bits = 0;
    req.h.msgh_size = sizeof(mach_msg_header_t) + payload;
    req.h.msgh_remote_port = FS_PORT;
    req.h.msgh_local_port = SHELL_PORT;      /* 应答回本端口 */
    req.h.msgh_id = id;
    req.h.msgh_reserved = 0;
    mach_msg_send(&req, req.h.msgh_size);
}

/* 阻塞等待 FS 应答（忽略期间的键盘字符） */
static void fs_wait_and_print(uint32_t expect_id, bool raw)
{
    for (;;) {
        if (mach_msg_recv(g_rx, sizeof(g_rx), SHELL_PORT) != MACH_MSG_SUCCESS) {
            return;
        }
        mach_msg_header_t *h = (mach_msg_header_t *)g_rx;
        if (h->msgh_id == MSG_ID_KEYCHAR) {
            continue;                        /* 等待期间丢弃按键 */
        }
        if (h->msgh_id != expect_id) {
            continue;
        }
        fs_resp_t *fr = (fs_resp_t *)(g_rx + sizeof(*h));
        char *data = (char *)g_rx + sizeof(*h) + sizeof(*fr);
        if (fr->status == FS_ERR_NOENT) {
            u_print("cat: file not found\n");
        } else if (fr->status != FS_OK) {
            u_print("fs: I/O error\n");
        } else {
            u_printn(data, fr->length);
            if (raw && fr->length > 0 && data[fr->length - 1] != '\n') {
                u_print("\n");
            }
        }
        return;
    }
}

/* 大写化（FAT32 8.3 名匹配用） */
static void upcase(char *s)
{
    for (; *s; s++) {
        if (*s >= 'a' && *s <= 'z') {
            *s = (char)(*s - 'a' + 'A');
        }
    }
}

static void run_command(char *line)
{
    /* 去前导空格 */
    while (*line == ' ') {
        line++;
    }
    if (!*line) {
        return;
    }

    /* 拆分命令与参数 */
    char *arg = line;
    while (*arg && *arg != ' ') {
        arg++;
    }
    if (*arg) {
        *arg++ = '\0';
        while (*arg == ' ') {
            arg++;
        }
    }

    if (u_strcmp(line, "help") == 0) {
        u_print("SukiOS shell (Ring3). Built-in commands:\n"
                "  help          - show this help\n"
                "  ls            - list FAT32 root directory (via FS_SERVER)\n"
                "  cat <FILE>    - print file content (8.3 name, e.g. README.TXT)\n"
                "  reboot        - reboot the machine\n");
    } else if (u_strcmp(line, "ls") == 0) {
        fs_request(FS_MSG_LIST, 0);
        fs_wait_and_print(FS_MSG_LIST, false);
    } else if (u_strcmp(line, "cat") == 0) {
        if (!*arg) {
            u_print("usage: cat <FILE>\n");
        } else {
            upcase(arg);
            fs_request(FS_MSG_READ, arg);
            fs_wait_and_print(FS_MSG_READ, true);
        }
    } else if (u_strcmp(line, "reboot") == 0) {
        u_print("rebooting...\n");
        suki_syscall5(SYS_REBOOT, 0, 0, 0, 0, 0);
    } else if (u_strcmp(line, "exec") == 0) {
        if (!*arg) {
            u_print("usage: exec <FILE> [args...]\n");
        } else {
            /* 把命令后的剩余字符串按空格拆成 argv[]，argv[0]=程序路径。
             * D2 修复（防御）：显式清零整个 argv 数组，保证即便参数个数
             * 触顶(ac<7)也不会有未初始化栈垃圾被内核误当作额外指针读取，
             * 从而避免 exec 传参 argv 错乱（审计 D2 项）。 */
            char *argv[8];
            for (int zi = 0; zi < 8; zi++) {
                argv[zi] = NULL;
            }
            int ac = 0;
            char *p = arg;
            while (*p && ac < 7) {
                while (*p == ' ') {
                    p++;
                }
                if (!*p) {
                    break;
                }
                argv[ac++] = p;
                while (*p && *p != ' ') {
                    p++;
                }
                if (*p) {
                    *p++ = '\0';
                }
            }
            argv[ac] = NULL;
            /* spawn 一个独立子任务运行该程序，当前 shell 阻塞等待其退出，
             * 子任务结束后 shell 重新接管（不会像 execve 那样被替换掉）。 */
            int pid = sys_task_spawn(argv[0], argv, NULL);
            if (pid < 0) {
                u_print("exec failed: file not found or invalid ELF\n");
            } else {
                uint64_t rc = sys_wait((uint64_t)pid);
                u_print("  [shell] child pid=");
                char db[24];
                u_print(u_utoa_s((uint64_t)pid, db, sizeof(db)));
                u_print(" exited (code=");
                u_print(u_utoa_s(rc, db, sizeof(db)));
                u_print(")\n");
            }
        }
    } else {
        u_print("unknown command: ");
        u_print(line);
        u_print("  (try 'help')\n");
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    char line[LINE_MAX];
    uint32_t len = 0;

    u_print("\n[shell] SukiOS shell online (Ring3, pid via IPC pipeline)\n");
    sys_port_claim(SHELL_PORT);    /* A2 项：认领 shell 接收端口 */
    u_print("Type 'help' for commands.\n\n");
    prompt();

    for (;;) {
        if (mach_msg_recv(g_rx, sizeof(g_rx), SHELL_PORT) != MACH_MSG_SUCCESS) {
            continue;
        }
        mach_msg_header_t *h = (mach_msg_header_t *)g_rx;
        if (h->msgh_id != MSG_ID_KEYCHAR) {
            continue;                        /* 迟到的 FS 应答等，忽略 */
        }
        char c = *((char *)g_rx + sizeof(*h));

        if (c == '\b') {
            if (len > 0) {
                len--;
                u_print("\b \b");
            }
            continue;
        }
        if (c == '\n') {
            u_print("\n");
            line[len] = '\0';
            run_command(line);
            len = 0;
            prompt();
            continue;
        }
        if (len < LINE_MAX - 1 && c >= 32 && c < 127) {
            line[len++] = c;
            u_printn(&c, 1);                 /* 回显 */
        }
    }
    return 0;
}
