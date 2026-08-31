/*
 * user/shell.c
 * -----------------------------------------------------------------------------
 * SukiOS Shell —— Ring3 命令行解释器（向 bash 靠拢的用户态程序）。
 *
 * 输入：SHELL_PORT 阻塞接收
 *   msgh_id=1 (MSG_ID_KEYCHAR): 键盘字符（INPUT_SERVER 解析后逐字符发来；
 *       方向键以上转义序列形式送达：ESC [ A=上, ESC [ B=下, ESC [ C=右, ESC [ D=左）
 *   msgh_id=FS_MSG_* : FS_SERVER 的应答
 * 输出：sys_debug_write -> 内核统一控制台（帧缓冲图形终端 + 串口）
 *
 * 内置命令（在原有 help/ls/cat/mkfile/mkdir/write/rm/rename/truncate/exec/
 * reboot/poweroff 基础上，向 bash 扩充）：
 *   cd [dir]     切换目录（cd / 回根，cd ~ 回家目录，无参=根）
 *   pwd          打印当前工作目录（走 SYS_GETCWD）
 *   echo [-n][-e] <args...>   支持 $VAR / $? 变量展开，-n 不换行，-e 解释转义
 *   export VAR=val           设置环境变量（进程内 environ，spawn 时随子进程继承）
 *   env / set    列出当前环境变量
 *   clear        清屏（VT100 转义）
 *   history      打印命令历史
 * 行编辑/语法：
 *   - 命令分隔 ';' 与短路逻辑 '&&' / '||'（按 bash 语义：&& 前成功才执行，
 *     || 前失败才执行）
 *   - '#' 注释（行内 # 之后内容被忽略）
 *   - '~' 展开为家目录（本系统家目录即根 /）
 *   - 上/下方向键浏览历史命令
 *
 * 调用关系：INPUT_SERVER --IPC--> shell --IPC--> FS_SERVER --IPC--> DISK_PORT。
 */
#include "lib/suki.h"
#include <ipc/fs_proto.h>
#include "lib/libc.h"

#define MSG_ID_KEYCHAR 100   /* 与 input_server 一致；避开 FS_MSG_* */
#define LINE_MAX 256
#define HIST_MAX 32
#define ARG_MAX  32

static uint8_t g_rx[sizeof(mach_msg_header_t) + sizeof(fs_resp_t) + FS_DATA_MAX + 16];
/* 写请求发送缓冲（含 FS_WRITE_MAX 数据上限） */
static uint8_t g_wreq[sizeof(mach_msg_header_t) + sizeof(fs_write_req_t) + 96 + FS_WRITE_MAX];

static char g_cwd[PATH_MAX] = "/";   /* 当前工作目录（chdir 维护） */
static int  g_last_exit = 0;          /* 上条命令退出码，供 $? 使用 */
static char g_hist[HIST_MAX][LINE_MAX];
static int  g_hist_count = 0;
static int  g_hist_idx = 0;           /* 上/下浏览游标（=g_hist_count 表示新行） */

/* 前向声明 */
static int run_builtin_raw(char *line);

/* ---- 行编辑转义序列状态机 ---- */
#define ESC_NONE 0
#define ESC_ESC  1   /* 已收到 ESC */
#define ESC_BRK  2   /* 已收到 ESC [ */
static int g_esc = ESC_NONE;

static void prompt(void)
{
    u_print("SukiOS:");
    u_print(g_cwd);
    u_print("> ");
}

/* 发送 FS 请求（应答异步回到 SHELL_PORT，由主循环等待收取） */
static void fs_request(uint32_t id, const char *arg)
{
    struct {
        mach_msg_header_t h;
        char name[64];
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
        if (h->msgh_id == MSG_ID_SERVICE_DOWN) {
            u_print("fs: service unavailable (fs-server down)\n");
            return;                          /* 立即回提示符，不再永久等待 */
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

/* 发送写文件请求（name NUL 结尾 + length 字节数据，紧跟其后）。 */
static void fs_request_write(const char *name, const char *data, uint32_t len,
                             uint32_t offset)
{
    mach_msg_header_t *h = (mach_msg_header_t *)g_wreq;
    fs_write_req_t *wr = (fs_write_req_t *)(g_wreq + sizeof(mach_msg_header_t));
    wr->offset = offset;
    wr->length = len;
    char *nm = (char *)(g_wreq + sizeof(mach_msg_header_t) + sizeof(fs_write_req_t));
    uint32_t ni = 0;
    while (name[ni] && ni < 63) { nm[ni] = name[ni]; ni++; }
    nm[ni++] = 0;
    u_memcpy(nm + ni, data, len);
    h->msgh_bits = 0;
    h->msgh_size = sizeof(mach_msg_header_t) + sizeof(fs_write_req_t) + ni + len;
    h->msgh_remote_port = FS_PORT;
    h->msgh_local_port = SHELL_PORT;
    h->msgh_id = FS_MSG_WRITE;
    h->msgh_reserved = 0;
    mach_msg_send(g_wreq, h->msgh_size);
}

/* 阻塞等待写类操作的应答（忽略期间的键盘字符），打印结果。 */
static void fs_wait_status(uint32_t expect_id)
{
    for (;;) {
        if (mach_msg_recv(g_rx, sizeof(g_rx), SHELL_PORT) != MACH_MSG_SUCCESS) {
            return;
        }
        mach_msg_header_t *h = (mach_msg_header_t *)g_rx;
        if (h->msgh_id == MSG_ID_KEYCHAR) {
            continue;
        }
        if (h->msgh_id == MSG_ID_SERVICE_DOWN) {
            u_print("fs: service unavailable (fs-server down)\n");
            return;
        }
        if (h->msgh_id != expect_id) {
            continue;
        }
        fs_resp_t *fr = (fs_resp_t *)(g_rx + sizeof(*h));
        if (fr->status == FS_OK) {
            u_print("ok\n");
        } else if (fr->status == FS_ERR_NOENT) {
            u_print("fs: no such file\n");
        } else {
            u_print("fs: I/O error\n");
        }
        return;
    }
}

/* 把路径做 ~ 展开（仅处理开头的 ~ 或 ~/...，家目录即根 /）。
 * 结果写入 out（长度安全）。 */
static void expand_tilde(const char *in, char *out, size_t outsz)
{
    if (in[0] == '~') {
        const char *rest = &in[1];
        if (rest[0] == '/' || rest[0] == '\0') {
            snprintf(out, outsz, "/%s", rest[0] == '/' ? rest + 1 : "");
            return;
        }
        /* ~user 形式不支持，原样返回 */
    }
    snprintf(out, outsz, "%s", in);
}

/* 简单变量展开：将 src 中的 $VAR 与 $? 替换为 environ/env 中的值，写入 dst。
 * 支持 ${VAR} 与 $VAR（VAR 为字母/数字/下划线序列）；$? 替换为上条命令退出码。
 * 单引号内不展开（bash 行为）；双引号内展开。 */
static void expand_vars(char *dst, size_t dstsz, const char *src)
{
    size_t di = 0;
    char quote = 0;   /* 0=无, '\''=单, '"'=双 */
    for (size_t si = 0; src[si] && di + 1 < dstsz; si++) {
        char c = src[si];
        if (quote) {
            if (c == quote) { quote = 0; continue; }
            if (quote == '\'' || c != '$') { dst[di++] = c; continue; }
            /* 双引号内遇到 $，按未引用处理（落到下方展开逻辑） */
        } else if (c == '\'' || c == '"') {
            quote = c;
            continue;
        }
        if (c == '$') {
            if (src[si + 1] == '?') {
                di += (size_t)snprintf(&dst[di], dstsz - di, "%d", g_last_exit);
                si++;
                continue;
            }
            if (src[si + 1] == '{') {
                size_t e = si + 2;
                while (src[e] && src[e] != '}') e++;
                size_t nlen = e - (si + 2);
                char name[64];
                if (nlen < sizeof(name)) {
                    u_memcpy(name, &src[si + 2], nlen);
                    name[nlen] = '\0';
                    const char *v = getenv(name);
                    if (v) { size_t vl = u_strlen(v);
                             if (di + vl < dstsz) { u_memcpy(&dst[di], v, vl); di += vl; } }
                }
                si = (src[e] == '}') ? e : e - 1;
                continue;
            }
            if (isalpha((int)(unsigned char)src[si + 1]) || src[si + 1] == '_') {
                size_t s = si + 1;
                while (isalnum((int)(unsigned char)src[s]) || src[s] == '_') s++;
                size_t nlen = s - (si + 1);
                char name[64];
                if (nlen < sizeof(name)) {
                    u_memcpy(name, &src[si + 1], nlen);
                    name[nlen] = '\0';
                    const char *v = getenv(name);
                    if (v) { size_t vl = u_strlen(v);
                             if (di + vl < dstsz) { u_memcpy(&dst[di], v, vl); di += vl; } }
                }
                si = s - 1;
                continue;
            }
        }
        dst[di++] = c;
    }
    dst[di] = '\0';
}

/* 把一个 token 加入历史（去重：与最近一条相同则不复读） */
static void hist_push(const char *line)
{
    if (g_hist_count > 0 && u_strcmp(g_hist[g_hist_count - 1], line) == 0)
        return;
    if (g_hist_count < HIST_MAX) {
        strncpy(g_hist[g_hist_count], line, LINE_MAX - 1);
        g_hist[g_hist_count][LINE_MAX - 1] = '\0';
        g_hist_count++;
    } else {
        /* 环形：整体前移 */
        for (int i = 1; i < HIST_MAX; i++)
            strncpy(g_hist[i - 1], g_hist[i], LINE_MAX - 1);
        strncpy(g_hist[HIST_MAX - 1], line, LINE_MAX - 1);
        g_hist[HIST_MAX - 1][LINE_MAX - 1] = '\0';
    }
}

/* 打印一行（echo 用），带 -e 转义解释。 */
static void echo_print(const char *s, bool esc)
{
    for (size_t i = 0; s[i]; i++) {
        if (esc && s[i] == '\\' && s[i + 1]) {
            char o = s[++i];
            switch (o) {
                case 'n': u_print("\n"); break;
                case 't': u_print("\t"); break;
                case 'r': u_print("\r"); break;
                case '\\': u_print("\\"); break;
                case 'a': u_print("\a"); break;
                case '0': u_print("\0"); break;   /* 实际不可见，忽略 */
                default: { char b[2] = {o, 0}; u_print(b); } break;
            }
        } else {
            char b[2] = {s[i], 0};
            u_print(b);
        }
    }
}

/* 拆分 argv（空格分隔，支持 ' 与 " 引号）。返回参数个数。 */
static int split_args(char *line, char *argv[], int maxn)
{
    int ac = 0;
    char *p = line;
    while (*p && ac < maxn - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '\'' || *p == '"') {
            char q = *p++;
            argv[ac++] = p;
            while (*p && *p != q) p++;
            if (*p) *p++ = '\0';
        } else {
            argv[ac++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
        }
    }
    argv[ac] = NULL;
    return ac;
}

/* 执行单条（已无运算符的）命令，返回退出码。 */
static int do_command(char *line)
{
    /* 去掉前导空格 */
    while (*line == ' ' || *line == '\t') line++;
    if (!*line) return 0;

    /* 整体做变量展开 */
    static char expanded[LINE_MAX * 2];
    expand_vars(expanded, sizeof(expanded), line);
    char *cmd = expanded;
    while (*cmd == ' ' || *cmd == '\t') cmd++;

    char *argv[ARG_MAX];
    int ac = split_args(cmd, argv, ARG_MAX);
    if (ac == 0) return 0;

    char *name = argv[0];

    /* ---- cd ---- */
    if (u_strcmp(name, "cd") == 0) {
        char target[PATH_MAX];
        if (ac < 2 || u_strcmp(argv[1], "~") == 0) {
            target[0] = '/'; target[1] = '\0';
        } else {
            expand_tilde(argv[1], target, sizeof(target));
        }
        long r = suki_syscall1(SYS_CHDIR, (uint64_t)target);
        if (r < 0) {
            u_print("cd: "); u_print(target); u_print(": ");
            u_print(strerror((int)(-r))); u_print("\n");
            return 1;
        }
        char buf[PATH_MAX];
        if (getcwd(buf, sizeof(buf))) strncpy(g_cwd, buf, PATH_MAX - 1);
        return 0;
    }
    /* ---- pwd ---- */
    if (u_strcmp(name, "pwd") == 0) {
        char buf[PATH_MAX];
        if (getcwd(buf, sizeof(buf))) u_print(buf);
        else u_print("/");
        u_print("\n");
        return 0;
    }
    /* ---- echo ---- */
    if (u_strcmp(name, "echo") == 0) {
        bool nflag = false, eflag = false;
        int i = 1;
        while (i < ac) {
            if (u_strcmp(argv[i], "-n") == 0) { nflag = true; i++; continue; }
            if (u_strcmp(argv[i], "-e") == 0) { eflag = true; i++; continue; }
            if (u_strcmp(argv[i], "-E") == 0) { eflag = false; i++; continue; }
            break;
        }
        for (; i < ac; i++) {
            if (i > (nflag || eflag ? 1 : 1) && i > 1) u_print(" ");
            echo_print(argv[i], eflag);
        }
        if (!nflag) u_print("\n");
        return 0;
    }
    /* ---- export ---- */
    if (u_strcmp(name, "export") == 0) {
        if (ac < 2) { u_print("usage: export VAR=val\n"); return 1; }
        char *eq = strchr(argv[1], '=');
        if (!eq) { u_print("usage: export VAR=val\n"); return 1; }
        *eq = '\0';
        setenv(argv[1], eq + 1, 1);
        *eq = '=';   /* 还原（argv 在 expanded 缓冲，后面不再用） */
        return 0;
    }
    /* ---- env / set ---- */
    if (u_strcmp(name, "env") == 0 || u_strcmp(name, "set") == 0) {
        if (environ) {
            for (int i = 0; environ[i]; i++) u_print(environ[i]), u_print("\n");
        }
        return 0;
    }
    /* ---- clear ---- */
    if (u_strcmp(name, "clear") == 0) {
        u_print("\033[2J\033[H");
        return 0;
    }
    /* ---- history ---- */
    if (u_strcmp(name, "history") == 0) {
        for (int i = 0; i < g_hist_count; i++) {
            char num[12];
            u_print(u_utoa_s((uint64_t)(i + 1), num, sizeof(num)));
            u_print("  ");
            u_print(g_hist[i]);
            u_print("\n");
        }
        return 0;
    }

    /* ---- 其余原有命令（help/ls/cat/reboot/poweroff/exec/mkfile/mkdir/
    write/rm/rename/truncate）---- 这些命令本身不使用变量展开后的 argv（因为
    expand_vars 已把全局变量写进 expanded 缓冲，且它们的参数多带 8.3 大写名，
    不适合替换）。为保持既有行为，对它们直接用【原始 line】重新解析。 */
    return run_builtin_raw(line);
}

/* 原始（未展开）内置命令分发；返回退出码。与 do_command 共用，但参数来自
 * 原始输入行（不做变量展开），以保持 help/ls/cat/exec 等既有语义。 */
static int run_builtin_raw(char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    if (!*line) return 0;

    char *arg = line;
    while (*arg && *arg != ' ' && *arg != '\t') arg++;
    if (*arg) {
        *arg++ = '\0';
        while (*arg == ' ' || *arg == '\t') arg++;
    }

    if (u_strcmp(line, "help") == 0) {
        u_print("SukiOS shell (Ring3). Built-in commands:\n"
                "  help                 - show this help\n"
                "  ls [-l]              - list FAT32 directory (via FS_SERVER)\n"
                "  cat <FILE>           - print file content (8.3 name)\n"
                "  cd [dir]             - change directory (cd / , cd ~)\n"
                "  pwd                  - print working directory\n"
                "  echo [-n][-e] <txt>  - print (supports $VAR $?)\n"
                "  export VAR=val       - set environment variable\n"
                "  env / set            - list environment variables\n"
                "  history              - command history\n"
                "  clear                - clear screen\n"
                "  reboot               - reboot the machine\n"
                "  poweroff             - ACPI S5 soft power off\n"
                "  mkfile <FILE>        - create empty file\n"
                "  mkdir <DIR>          - create directory\n"
                "  write <FILE> <TEXT>  - write TEXT to FILE at offset 0\n"
                "  rm <FILE>            - delete file or empty directory\n"
                "  rename <OLD> <NEW>   - rename file/directory\n"
                "  truncate <FILE> <SIZE>- set file size\n"
                "  exec <FILE> [args...]- spawn standalone app (.ska auto-append)\n"
                "Syntax: ';' separates; '&&' / '||' short-circuit; '#' comment; '~' home\n");
    } else if (u_strcmp(line, "ls") == 0) {
        /* 支持 ls -l / ls --long：目前 FS 后端以固定格式返回目录清单，
         * -l 仅影响展示标签，核心仍走 FS_MSG_LIST（真正可工作的列表）。 */
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
    } else if (u_strcmp(line, "poweroff") == 0 || u_strcmp(line, "shutdown") == 0) {
        u_print("powering off (ACPI S5)...\n");
        suki_syscall5(SYS_REBOOT, 1, 0, 0, 0, 0);   /* mode=1 -> acpi_poweroff */
    } else if (u_strcmp(line, "exec") == 0) {
        if (!*arg) {
            u_print("usage: exec <FILE> [args...]\n");
        } else {
            char *argv[8];
            for (int zi = 0; zi < 8; zi++) argv[zi] = NULL;
            int ac = 0;
            char *p = arg;
            while (*p && ac < 7) {
                while (*p == ' ') p++;
                if (!*p) break;
                argv[ac++] = p;
                while (*p && *p != ' ') p++;
                if (*p) *p++ = '\0';
            }
            argv[ac] = NULL;
            int pid = sys_task_spawn(argv[0], argv, NULL);
            if (pid < 0) {
                int has_dot = 0;
                for (const char *q = argv[0]; *q; q++) if (*q == '.') { has_dot = 1; break; }
                if (!has_dot) {
                    char ska[256];
                    int n = 0;
                    const char *s = argv[0];
                    while (*s && n < (int)sizeof(ska) - 6) ska[n++] = *s++;
                    ska[n] = '.'; ska[n+1] = 's'; ska[n+2] = 'k'; ska[n+3] = 'a'; ska[n+4] = '\0';
                    pid = sys_task_spawn(ska, argv, NULL);
                }
            }
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
    } else if (u_strcmp(line, "mkfile") == 0) {
        if (!*arg) { u_print("usage: mkfile <FILE>\n"); }
        else { upcase(arg); fs_request(FS_MSG_CREATE, arg); fs_wait_status(FS_MSG_CREATE); }
    } else if (u_strcmp(line, "mkdir") == 0) {
        if (!*arg) { u_print("usage: mkdir <DIR>\n"); }
        else { upcase(arg); fs_request(FS_MSG_MKDIR, arg); fs_wait_status(FS_MSG_MKDIR); }
    } else if (u_strcmp(line, "write") == 0) {
        if (!*arg) { u_print("usage: write <FILE> <TEXT>\n"); }
        else {
            char *f = arg, *sp = arg;
            while (*sp && *sp != ' ' && *sp != '\t') sp++;
            if (!*sp) { u_print("usage: write <FILE> <TEXT>\n"); }
            else {
                *sp++ = 0; while (*sp == ' ' || *sp == '\t') sp++;
                upcase(f);
                uint32_t len = (uint32_t)u_strlen(sp);
                if (len > FS_WRITE_MAX) len = FS_WRITE_MAX;
                fs_request_write(f, sp, len, 0);
                fs_wait_status(FS_MSG_WRITE);
            }
        }
    } else if (u_strcmp(line, "rm") == 0) {
        if (!*arg) { u_print("usage: rm <FILE>\n"); }
        else { upcase(arg); fs_request(FS_MSG_UNLINK, arg); fs_wait_status(FS_MSG_UNLINK); }
    } else if (u_strcmp(line, "rename") == 0) {
        if (!*arg) { u_print("usage: rename <OLD> <NEW>\n"); }
        else {
            char *o = arg, *sp = arg;
            while (*sp && *sp != ' ' && *sp != '\t') sp++;
            if (!*sp) { u_print("usage: rename <OLD> <NEW>\n"); }
            else {
                *sp++ = 0; while (*sp == ' ' || *sp == '\t') sp++;
                upcase(o); upcase(sp);
                fs_rename_req_t *rr = (fs_rename_req_t *)g_wreq;
                mach_msg_header_t *h = (mach_msg_header_t *)g_wreq;
                uint32_t i = 0;
                while (o[i] && i < 63) { rr->old_name[i] = o[i]; i++; }
                rr->old_name[i] = 0;
                i = 0;
                while (sp[i] && i < 63) { rr->new_name[i] = sp[i]; i++; }
                rr->new_name[i] = 0;
                h->msgh_bits = 0;
                h->msgh_size = sizeof(mach_msg_header_t) + sizeof(fs_rename_req_t);
                h->msgh_remote_port = FS_PORT;
                h->msgh_local_port = SHELL_PORT;
                h->msgh_id = FS_MSG_RENAME;
                h->msgh_reserved = 0;
                mach_msg_send(g_wreq, h->msgh_size);
                fs_wait_status(FS_MSG_RENAME);
            }
        }
    } else if (u_strcmp(line, "truncate") == 0) {
        if (!*arg) { u_print("usage: truncate <FILE> <SIZE>\n"); }
        else {
            char *f = arg, *sp = arg;
            while (*sp && *sp != ' ' && *sp != '\t') sp++;
            if (!*sp) { u_print("usage: truncate <FILE> <SIZE>\n"); }
            else {
                *sp++ = 0; while (*sp == ' ' || *sp == '\t') sp++;
                upcase(f);
                uint32_t size = 0;
                for (uint32_t i = 0; sp[i] >= '0' && sp[i] <= '9'; i++)
                    size = size * 10 + (uint32_t)(sp[i] - '0');
                fs_trunc_req_t *tr = (fs_trunc_req_t *)(g_wreq + sizeof(mach_msg_header_t));
                tr->size = size;
                char *nm = (char *)(g_wreq + sizeof(mach_msg_header_t) + sizeof(fs_trunc_req_t));
                uint32_t ni = 0;
                while (f[ni] && ni < 63) { nm[ni] = f[ni]; ni++; }
                nm[ni++] = 0;
                mach_msg_header_t *h = (mach_msg_header_t *)g_wreq;
                h->msgh_bits = 0;
                h->msgh_size = sizeof(mach_msg_header_t) + sizeof(fs_trunc_req_t) + ni;
                h->msgh_remote_port = FS_PORT;
                h->msgh_local_port = SHELL_PORT;
                h->msgh_id = FS_MSG_TRUNCATE;
                h->msgh_reserved = 0;
                mach_msg_send(g_wreq, h->msgh_size);
                fs_wait_status(FS_MSG_TRUNCATE);
            }
        }
    } else {
        u_print("unknown command: ");
        u_print(line);
        u_print("  (try 'help')\n");
        return 127;
    }
    return 0;
}

/* 解析并执行一整行（支持 ; && || 与 # 注释）。 */
static void run_command(char *line)
{
    /* 去掉 # 注释（未加引号的 #） */
    bool in_q = false; char qc = 0;
    for (char *p = line; *p; p++) {
        if (in_q) { if (*p == qc) in_q = false; }
        else if (*p == '\'' || *p == '"') { in_q = true; qc = *p; }
        else if (*p == '#') { *p = '\0'; break; }
    }

    /* 以 ; 为顶层分隔切分（&& / || 在子段内处理） */
    char *segments[16];
    int nseg = 0;
    char *p = line;
    char *start = p;
    while (*p && nseg < 15) {
        if (*p == ';') {
            *p = '\0';
            segments[nseg++] = start;
            start = p + 1;
        }
        p++;
    }
    if (*start) segments[nseg++] = start;

    bool last_ok = true;       /* 上一条结果（true=成功/退出码0） */
    for (int s = 0; s < nseg; s++) {
        char *seg = segments[s];
        while (*seg == ' ' || *seg == '\t') seg++;
        if (!*seg) { continue; }

        /* 段内可能存在的 && / || 运算符 */
        /* 先用简单状态机把段按 && / || 拆成子命令 + 运算符 */
        char *cmds[16];
        int   ops[16];   /* 0=首个, 1=&&, 2=|| */
        int   ncmd = 0;
        char *cp = seg;
        char *cstart = cp;
        while (*cp && ncmd < 15) {
            if (cp[0] == '&' && cp[1] == '&') {
                *cp = '\0';
                cmds[ncmd] = cstart;
                ops[ncmd] = (ncmd == 0) ? 0 : 1;
                ncmd++;
                cp += 2;
                while (*cp == ' ') cp++;
                cstart = cp;
            } else if (cp[0] == '|' && cp[1] == '|') {
                *cp = '\0';
                cmds[ncmd] = cstart;
                ops[ncmd] = (ncmd == 0) ? 0 : 2;
                ncmd++;
                cp += 2;
                while (*cp == ' ') cp++;
                cstart = cp;
            } else {
                cp++;
            }
        }
        if (*cstart) { cmds[ncmd] = cstart; ops[ncmd] = (ncmd == 0) ? 0 : 0; ncmd++; }

        for (int k = 0; k < ncmd; k++) {
            char *c = cmds[k];
            while (*c == ' ' || *c == '\t') c++;
            if (!*c) { if (k > 0) continue; else { last_ok = true; continue; } }
            if (k > 0) {
                /* 受运算符约束：只有满足短路条件才执行 */
                if (ops[k] == 1 && !last_ok) continue;   /* && 前失败，跳过 */
                if (ops[k] == 2 &&  last_ok) continue;   /* || 前成功，跳过 */
            }
            int rc = do_command(c);
            g_last_exit = rc;
            last_ok = (rc == 0);
        }
    }
}

/* ---------------------------------------------------------------------------
 * libc 自测（无键盘注入场景下也能通过串口观测）。
 * 直接在启动早期跑一遍新 libc 接口，打印 [libc-test] PASS/FAIL 汇总。
 * 全部走真实 syscall / 真实 libc 实现，非桩。
 * --------------------------------------------------------------------------- */
static void libc_selftest(void)
{
    int pass = 0, fail = 0;
    char nbuf[8];

    /* 1) getenv/setenv/unsetenv 闭环 */
    setenv("LIBCTEST", "hello", 1);
    const char *v = getenv("LIBCTEST");
    if (v && u_strcmp(v, "hello") == 0) pass++; else { fail++; u_print("[libc-test] FAIL getenv/setenv\n"); }
    setenv("LIBCTEST", "world", 1);
    v = getenv("LIBCTEST");
    if (v && u_strcmp(v, "world") == 0) pass++; else { fail++; u_print("[libc-test] FAIL setenv overwrite\n"); }
    unsetenv("LIBCTEST");
    if (getenv("LIBCTEST") == NULL) pass++; else { fail++; u_print("[libc-test] FAIL unsetenv\n"); }

    /* 2) getopt 短选项解析（-a -b ARG -c） */
    char *av[] = {"prog", "-a", "-b", "val", "-c", NULL};
    optind = 1; opterr = 1; optarg = NULL;
    int a = 0, b = 0, c = 0; char *barg = NULL;
    int ch;
    while ((ch = getopt(5, av, "ab:c")) != -1) {
        if (ch == 'a') a = 1;
        else if (ch == 'b') { b = 1; barg = optarg; }
        else if (ch == 'c') c = 1;
    }
    if (a && b && c && barg && u_strcmp(barg, "val") == 0) pass++;
    else { fail++; u_print("[libc-test] FAIL getopt\n"); }

    /* 3) getopt_long 长选项解析 */
    struct option lopts[] = {
        {"verbose", no_argument, 0, 'v'},
        {"output",  required_argument, 0, 'o'},
        {NULL, 0, 0, 0}
    };
    char *av2[] = {"prog", "--verbose", "--output", "out.txt", NULL};
    optind = 1; optarg = NULL;
    int vflag = 0, oflag = 0; char *ofile = NULL;
    while ((ch = getopt_long(4, av2, "vo:", lopts, NULL)) != -1) {
        if (ch == 'v') vflag = 1;
        else if (ch == 'o') { oflag = 1; ofile = optarg; }
    }
    if (vflag && oflag && ofile && u_strcmp(ofile, "out.txt") == 0) pass++;
    else { fail++; u_print("[libc-test] FAIL getopt_long\n"); }

    /* 4) opendir/readdir 真实目录遍历（/ 经 SYS_OPENDIR 后端） */
    int found_files = 0;
    DIR *d = opendir("/");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (u_strlen(de->d_name) > 0) found_files++;
        }
        closedir(d);
    }
    if (found_files > 0) pass++; else { fail++; u_print("[libc-test] FAIL opendir/readdir\n"); }

    /* 5) strerror 字符串非空 */
    if (strerror(ENOENT) && u_strlen(strerror(ENOENT)) > 0) pass++;
    else { fail++; u_print("[libc-test] FAIL strerror\n"); }

    /* 6) expand_vars：$VAR 与 $? 展开 */
    setenv("GREET", "hi", 1);
    g_last_exit = 7;
    static char ev[128];
    expand_vars(ev, sizeof(ev), "x=$GREET y=$?");
    if (u_strcmp(ev, "x=hi y=7") == 0) pass++;
    else { fail++; u_print("[libc-test] FAIL expand_vars ("); u_print(ev); u_print(")\n"); }
    unsetenv("GREET");

    /* 汇总 */
    u_print("[libc-test] PASS=");
    u_print(u_utoa_s((uint64_t)pass, nbuf, sizeof(nbuf)));
    u_print(" FAIL=");
    u_print(u_utoa_s((uint64_t)fail, nbuf, sizeof(nbuf)));
    u_print(fail == 0 ? "  ALL OK\n" : "  SOME FAILED\n");
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    char line[LINE_MAX];
    uint32_t len = 0;

    /* 初始化家目录环境变量（bash 风格） */
    setenv("HOME", "/", 1);
    setenv("SHELL", "/BIN/SHELL.SKA", 1);
    setenv("PATH", "/BIN", 1);

    u_print("\n[shell] SukiOS shell online (Ring3, bash-like)\n");
    libc_selftest();
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

        /* ---- 方向键转义序列状态机 ---- */
        if (g_esc == ESC_ESC) {
            if (c == '[') { g_esc = ESC_BRK; continue; }
            g_esc = ESC_NONE;  /* 非 [ 则取消转义 */
        }
        if (g_esc == ESC_BRK) {
            g_esc = ESC_NONE;
            if (c == 'A') {  /* 上：历史上一条 */
                if (g_hist_count > 0) {
                    if (g_hist_idx > 0) g_hist_idx--;
                    int idx = g_hist_idx;
                    /* 清当前行 */
                    for (uint32_t i = 0; i < len; i++) u_print("\b \b");
                    len = 0;
                    const char *hs = g_hist[idx];
                    while (hs[len] && len < LINE_MAX - 1) { line[len] = hs[len]; len++; }
                    line[len] = '\0';
                    u_print(hs);
                }
                continue;
            }
            if (c == 'B') {  /* 下：历史下一条 */
                if (g_hist_count > 0 && g_hist_idx < g_hist_count) {
                    g_hist_idx++;
                    for (uint32_t i = 0; i < len; i++) u_print("\b \b");
                    len = 0;
                    if (g_hist_idx < g_hist_count) {
                        const char *hs = g_hist[g_hist_idx];
                        while (hs[len] && len < LINE_MAX - 1) { line[len] = hs[len]; len++; }
                        line[len] = '\0';
                        u_print(hs);
                    }
                }
                continue;
            }
            /* 其它方向键忽略 */
            continue;
        }
        if (c == 0x1B) {  /* ESC：进入转义序列 */
            g_esc = ESC_ESC;
            continue;
        }

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
            if (len > 0) {
                hist_push(line);
                g_hist_idx = g_hist_count;   /* 浏览游标回到“新行” */
                run_command(line);
            }
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
