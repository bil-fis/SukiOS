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
#include "lib/suki_gui.h"   /* 窗口化：shell 作为 WM 管理的窗口程序 */
#include "lib/gui_ipc.h"
#include "lib/font8x8.h"
#include <zlib.h>           /* miniz zlib 兼容层（user/lib/shims/zlib.h）验证用 */

/* 前向声明：把用户 TTY 环形管道中累计的文本渲染进终端窗口（定义在文件后方） */
static void DrainTtyPipe(void);

#define MSG_ID_KEYCHAR 100   /* 与 input_server 一致；避开 FS_MSG_* */
#define LINE_MAX 256
#define HIST_MAX 100   /* 内存历史最大条数（环形 FIFO，超出丢旧；不落盘） */
#define ARG_MAX  32

static uint8_t g_rx[sizeof(mach_msg_header_t) + sizeof(fs_resp_t) + FS_DATA_MAX + 16];
/* 写请求发送缓冲（含 FS_WRITE_MAX 数据上限） */
static uint8_t g_wreq[sizeof(mach_msg_header_t) + sizeof(fs_write_req_t) + 96 + FS_WRITE_MAX];

static char g_cwd[PATH_MAX] = "/";   /* 当前工作目录（chdir 维护） */
static int  g_last_exit = 0;          /* 上条命令退出码，供 $? 使用 */
static char g_hist[HIST_MAX][LINE_MAX];
static int  g_hist_count = 0;
static int  g_hist_idx = 0;           /* 上/下浏览游标（=g_hist_count 表示新行） */

/* 行内编辑状态：当前行缓冲 + 光标下标（支持任意位置插入/删除，bash 风格） */
static char g_line[LINE_MAX];
static int  g_len = 0;    /* 已输入字符数 */
static int  g_cur = 0;    /* 光标下标 0..g_len */

/* 终端光标列追踪（与显示服务侧的 g_term_x 保持同步的「用户态镜像」）。
 * shell 在行编辑期只做「整行重绘」，重绘前必须先把终端光标移回编辑行起点，
 * 否则 ESC[K 会从错误的列开始清屏、把已删除字符残留屏幕上（退格失效的根因）。
 * 本镜像在 prompt() 时初始化为提示符之后的列号，并由 ed_raw() 在每次输出时
 * 随字符/转义序列同步推进，使 redraw_from_cursor() 总能算出「回退到行首」所需的
 * 左移量，从而不依赖显示服务回复、也无需绝对定位（避免跨行环绕问题）。 */
static int  g_scr_x = 0;        /* 终端光标当前列（相对整行左缘） */
static int  g_prompt_col = 0;   /* 编辑行起点列（提示符之后） */

/* ---- 窗口化：shell 作为 WM 管理的窗口程序 ----
 * 启动后创建窗口并注册事件端口；键盘经 WM 转发到本窗口（焦点模型），
 * 输出渲染到窗口离屏缓冲（同时镜像 serial，便于无图形/headless 观测）。
 * 窗口尺寸与字符网格已放大（OOL 上限提升至 256 页=2MiB），不再受 128×128 限制。 */
static suki_window_t *g_win = NULL;
static uint32_t       g_ep  = 0;
#define TERM_COLS 80
#define TERM_ROWS 34
static char     g_tgrid[TERM_ROWS][TERM_COLS];
static int      g_tcx = 0, g_tcy = 0;   /* 终端网格光标（列/行） */
static bool     g_term_dirty = false;

/* 前向声明 */
static int run_builtin_raw(char *line);
static void prompt(void);
static void term_puts(const char *s);   /* 终端仿真输出（定义见下方窗口化段） */
static void shell_out(const char *s);    /* 双写：终端网格 + 串口镜像 */
static void shell_outn(const char *s, size_t n);

/* ============ 行内编辑（光标感知） ============ */
/* ---- 终端光标列 g_scr_x 由 term_emit_char 直接维护（见下方终端仿真） ---- */
static void ed_raw(const char *s) { term_puts(s); }

/* 发射左/右移光标的 CSI 序列（经 ed_raw 同步镜像） */
static void ed_left(int n)
{
    if (n <= 0) return;
    char seq[16]; int k = 0;
    seq[k++] = '\x1b'; seq[k++] = '[';
    if (n >= 10) seq[k++] = '0' + (n / 10);
    seq[k++] = '0' + (n % 10);
    seq[k++] = 'D';
    ed_raw(seq);
}
static void ed_right(int n)
{
    if (n <= 0) return;
    char seq[16]; int k = 0;
    seq[k++] = '\x1b'; seq[k++] = '[';
    if (n >= 10) seq[k++] = '0' + (n / 10);
    seq[k++] = '0' + (n % 10);
    seq[k++] = 'C';
    ed_raw(seq);
}

/* 用【绝对列定位】重绘整行。
 * 做法：先以 CHA(ESC[<col>G) 跳到编辑行起点列，再以 ESC[K 清到行尾（保留提示符），
 * 重印整行，最后把光标定位到 g_cur。
 *
 * 为什么必须绝对定位：之前（step55/56）用 ed_left(g_scr_x - g_prompt_col) 相对回退到行首，
 * 这要求 g_scr_x 与显示侧 g_term_x 绝对同步。但凡命令输出未以换行结尾、或 clear 之后
 * 终端光标落在非预期列，二者就会失步——清行尾从错误列开始，旧字符（典型如 "lsear" 里
 * 的 "ear"）无法被完整清除。绝对列定位完全不依赖 g_scr_x，每次都落在当前编辑行的正确
 * 列，因此无论之前光标被带偏到哪，ESC[K 都从编辑行起点清起，残留必被抹掉。 */
static void redraw_full_line(void)
{
    char seq[16]; int k;
    k = 0; seq[k++] = '\x1b'; seq[k++] = '[';
    int col = g_prompt_col + 1;                  /* 1-based 绝对列 */
    if (col >= 10) seq[k++] = '0' + col / 10;
    seq[k++] = '0' + col % 10;
    seq[k++] = 'G';
    ed_raw(seq);                                 /* 跳到编辑行起点列 */
    ed_raw("\x1b[K");                            /* 清到行尾（保留提示符） */
    ed_raw(g_line);                              /* 重印整行 */
    k = 0; seq[k++] = '\x1b'; seq[k++] = '[';    /* 光标定位到 g_cur */
    col = g_prompt_col + g_cur + 1;
    if (col >= 10) seq[k++] = '0' + col / 10;
    seq[k++] = '0' + col % 10;
    seq[k++] = 'G';
    ed_raw(seq);
    g_scr_x = g_prompt_col + g_cur;              /* 同步镜像，供 edit_* 相对移动使用 */
}

/* 从光标处起重绘（保持光标位置）——统一走 redraw_full_line 的绝对列定位版本 */
static void redraw_from_cursor(void)
{
    redraw_full_line();
}

/* 在光标处插入一个字符（bash 风格：把右侧字符右推） */
static void edit_insert(char c)
{
    if (g_len >= LINE_MAX - 1) return;
    if (g_cur < g_len)
        memmove(g_line + g_cur + 1, g_line + g_cur, g_len - g_cur);
    g_line[g_cur] = c;
    g_len++; g_cur++;
    g_line[g_len] = '\0';   /* 维持 C 串终止，避免旧命令残留字节被 redraw_full_line 印出 */
    redraw_from_cursor();   /* 统一整行重绘，正确反映插入字符与光标位置 */
}

static void edit_backspace(void)           /* 删光标前一个字符 */
{
    if (g_cur <= 0) return;
    g_cur--;
    memmove(g_line + g_cur, g_line + g_cur + 1, g_len - g_cur);
    g_len--;
    g_line[g_len] = '\0';   /* 截断，避免删后旧字符残留被重绘 */
    redraw_from_cursor();
}

static void edit_delete(void)              /* 删光标后一个字符 (Del) */
{
    if (g_cur >= g_len) return;
    memmove(g_line + g_cur, g_line + g_cur + 1, g_len - g_cur);
    g_len--;
    g_line[g_len] = '\0';   /* 截断，避免删后旧字符残留被重绘 */
    redraw_from_cursor();
}

static void edit_left(void)  { if (g_cur > 0)  { g_cur--; ed_left(1); } }
static void edit_right(void) { if (g_cur < g_len) { g_cur++; ed_right(1); } }
static void edit_home(void)
{
    if (g_cur > 0) {
        ed_left(g_cur);     /* 镜像列已含起点偏移：左移 g_cur 列即回行首 */
        g_cur = 0;
    }
}
static void edit_end(void)
{
    if (g_cur < g_len) {
        ed_right(g_len - g_cur);
        g_cur = g_len;
    }
}

/* 用 text 整体替换当前编辑行（用于历史切换）。text 为 "" 表示回到空的新行。
 *
 * 关键：必须【先】按【当前镜像】把终端光标左移回行首（CLI 下不能用旧 g_cur
 * 计算左移量——屏幕光标实际停在旧行重印之后的位置），再清行、打印新内容。
 * 用镜像列 g_scr_x 计算偏移可彻底避免历史切换时旧命令残留/错位。 */
static void line_replace(const char *text)
{
    int back = g_scr_x - g_prompt_col;     /* 终端光标距编辑行起点的偏移 */
    if (back > 0) ed_left(back);           /* 1) 左移回行首 */
    ed_raw("\x1b[K");                      /* 2) 清整行 */
    if (text) {                            /* 3) 载入并打印新内容 */
        int i = 0;
        while (text[i] && i < LINE_MAX - 1) { g_line[i] = text[i]; i++; }
        g_line[i] = '\0';
        g_len = i;
    } else {
        g_line[0] = '\0';
        g_len = 0;
    }
    g_cur = g_len;
    redraw_full_line();
}

/* Tab 补全：对当前光标所在词的目录/基础名前缀做文件名匹配。
 * 唯一匹配直接补全（目录补 '/'）；多匹配先补公共前缀再列出候选。 */
static void edit_tab_complete(void)
{
    /* 取当前词（光标前的最后一段，按空格/重定向/管道切分） */
    int ws = g_cur;
    while (ws > 0 &&
           g_line[ws-1] != ' ' && g_line[ws-1] != '>' &&
           g_line[ws-1] != '|' && g_line[ws-1] != '\n')
        ws--;
    int fraglen = g_cur - ws;
    if (fraglen == 0) return;

    char frag[LINE_MAX];
    memcpy(frag, g_line + ws, fraglen); frag[fraglen] = 0;

    /* 拆目录部分与基础名 */
    char dir[PATH_MAX]; char base[LINE_MAX];
    const char *slash = strrchr(frag, '/');
    if (slash) {
        int dlen = (int)(slash - frag);
        memcpy(dir, frag, dlen); dir[dlen] = 0;
        strcpy(base, slash + 1);
        if (dir[0] != '/') {
            /* 相对路径：仅拼接【目录部分】dir，而非整个 frag——否则会把基础名
             * 也拼进目录路径，导致 opendir 失败（如 "SUB/F" 被拼成 "/SUB/F"）。 */
            char tmp[PATH_MAX];
            snprintf(tmp, sizeof(tmp), "%s%s%s", g_cwd,
                     (g_cwd[0] && g_cwd[strlen(g_cwd)-1] != '/') ? "/" : "",
                     dir);
            strncpy(dir, tmp, sizeof(dir) - 1);
            dir[sizeof(dir) - 1] = '\0';
        }
    } else {
        strncpy(dir, g_cwd, sizeof dir); dir[sizeof(dir)-1] = 0;
        strcpy(base, frag);
    }

    /* 枚举目录匹配项 */
    char matches[64][LINE_MAX];
    uint8_t matchtype[64];
    int nmatch = 0;
    DIR *dp = opendir(dir);
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp)) != NULL && nmatch < 64) {
            if (de->d_name[0] == '.' && base[0] != '.') continue;
            if (strncmp(de->d_name, base, strlen(base)) == 0) {
                strncpy(matches[nmatch], de->d_name, LINE_MAX - 1);
                matches[nmatch][LINE_MAX - 1] = 0;
                matchtype[nmatch] = de->d_type;
                nmatch++;
            }
        }
        closedir(dp);
    }
    if (nmatch == 0) return;

    if (nmatch == 1) {
        char add[LINE_MAX];
        bool isdir = (matches[0][0] && matchtype[0] == DT_DIR);
        snprintf(add, sizeof add, "%s%s", matches[0] + strlen(base),
                 isdir ? "/" : "");
        for (int i = 0; add[i]; i++) edit_insert(add[i]);
        return;
    }
    /* 多匹配：补公共前缀 */
    int common = strlen(base);
    for (;;) {
        char ch = matches[0][common];
        if (!ch) break;
        bool same = true;
        for (int i = 1; i < nmatch; i++)
            if (matches[i][common] != ch) { same = false; break; }
        if (!same) break;
        common++;
    }
    if (common > (int)strlen(base)) {
        char add[LINE_MAX];
        snprintf(add, sizeof add, "%s", matches[0] + strlen(base));
        add[common - strlen(base)] = 0;
        for (int i = 0; add[i]; i++) edit_insert(add[i]);
    }
    /* 列出候选 */
    shell_out("\r\n");
    for (int i = 0; i < nmatch; i++) {
        shell_out("  "); shell_out(matches[i]); shell_out("\r\n");
    }
    prompt();
    /* prompt() 之后光标位于编辑行起点（提示符之后），g_scr_x 已重置。
     * 用绝对列重绘整行，保证与显示侧一致、无残留。 */
    g_cur = g_len;
    redraw_full_line();
}


/* ---- 行编辑转义序列状态机 ---- */
#define ESC_NONE 0
#define ESC_ESC  1   /* 已收到 ESC */
#define ESC_BRK  2   /* 已收到 ESC [ */
static int g_esc = ESC_NONE;
static int g_esc_num = 0;   /* ESC[ 之后的数字前缀（如 Delete 的 3） */

static void prompt(void)
{
    /* 关键：强制「回车 + 换行」让终端光标回到新行行首，并重置镜像。
     * 否则若上一条命令输出未以换行结尾（如 echo -n、文件末尾无 \n 的 cat、
     * 或 FS 列表恰在行尾收尾），显示侧光标会停在任意列，而 g_scr_x 仍按
     * g_prompt_col 计算，二者失步 → redraw_from_cursor() 清行尾的起点算错，
     * 旧字符残留（如 "lsear"）。先 \r\n 复位可彻底规避这一类失步。 */
    ed_raw("\r\n");
    g_scr_x = 0;
    shell_out("SukiOS:");
    shell_out(g_cwd);
    shell_out("> ");
    /* 编辑行起点列 = 提示符长度（"SukiOS:" = 7 + "> " = 2 = 9）+ cwd 长度。
     * 同时初始化终端列镜像，使后续 redraw_from_cursor() 的「回退到行首」计算正确。 */
    g_prompt_col = 9 + (int)strlen(g_cwd);
    g_scr_x = g_prompt_col;
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
            shell_out("fs: service unavailable (fs-server down)\n");
            return;                          /* 立即回提示符，不再永久等待 */
        }
        if (h->msgh_id != expect_id) {
            continue;
        }
        fs_resp_t *fr = (fs_resp_t *)(g_rx + sizeof(*h));
        char *data = (char *)g_rx + sizeof(*h) + sizeof(*fr);
        if (fr->status == FS_ERR_NOENT) {
            shell_out("cat: file not found\n");
        } else if (fr->status != FS_OK) {
            shell_out("fs: I/O error\n");
        } else {
            shell_outn(data, fr->length);
            if (raw && fr->length > 0 && data[fr->length - 1] != '\n') {
                shell_out("\n");
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
            shell_out("fs: service unavailable (fs-server down)\n");
            return;
        }
        if (h->msgh_id != expect_id) {
            continue;
        }
        fs_resp_t *fr = (fs_resp_t *)(g_rx + sizeof(*h));
        if (fr->status == FS_OK) {
            shell_out("ok\n");
        } else if (fr->status == FS_ERR_NOENT) {
            shell_out("fs: no such file\n");
        } else {
            shell_out("fs: I/O error\n");
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
                case 'n': shell_out("\n"); break;
                case 't': shell_out("\t"); break;
                case 'r': shell_out("\r"); break;
                case '\\': shell_out("\\"); break;
                case 'a': shell_out("\a"); break;
                case '0': shell_out("\0"); break;   /* 实际不可见，忽略 */
                default: { char b[2] = {o, 0}; shell_out(b); } break;
            }
        } else {
            char b[2] = {s[i], 0};
            shell_out(b);
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
            shell_out("cd: "); shell_out(target); shell_out(": ");
            shell_out(strerror((int)(-r))); shell_out("\n");
            return 1;
        }
        char buf[PATH_MAX];
        if (getcwd(buf, sizeof(buf))) strncpy(g_cwd, buf, PATH_MAX - 1);
        return 0;
    }
    /* ---- pwd ---- */
    if (u_strcmp(name, "pwd") == 0) {
        char buf[PATH_MAX];
        if (getcwd(buf, sizeof(buf))) shell_out(buf);
        else shell_out("/");
        shell_out("\n");
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
            if (i > (nflag || eflag ? 1 : 1) && i > 1) shell_out(" ");
            echo_print(argv[i], eflag);
        }
        if (!nflag) shell_out("\n");
        return 0;
    }
    /* ---- export ---- */
    if (u_strcmp(name, "export") == 0) {
        if (ac < 2) { shell_out("usage: export VAR=val\n"); return 1; }
        char *eq = strchr(argv[1], '=');
        if (!eq) { shell_out("usage: export VAR=val\n"); return 1; }
        *eq = '\0';
        setenv(argv[1], eq + 1, 1);
        *eq = '=';   /* 还原（argv 在 expanded 缓冲，后面不再用） */
        return 0;
    }
    /* ---- env / set ---- */
    if (u_strcmp(name, "env") == 0 || u_strcmp(name, "set") == 0) {
        if (environ) {
            for (int i = 0; environ[i]; i++) shell_out(environ[i]), shell_out("\n");
        }
        return 0;
    }
    /* ---- clear ---- */
    if (u_strcmp(name, "clear") == 0) {
        shell_out("\033[2J\033[H");
        return 0;
    }
    /* ---- history ---- */
    if (u_strcmp(name, "history") == 0) {
        for (int i = 0; i < g_hist_count; i++) {
            char num[12];
            shell_out(u_utoa_s((uint64_t)(i + 1), num, sizeof(num)));
            shell_out("  ");
            shell_out(g_hist[i]);
            shell_out("\n");
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
        shell_out("SukiOS shell (Ring3). Built-in commands:\n"
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
         * -l 仅影响展示标签，核心仍走 FS_MSG_LIST（真正可工作的列表）。
         * 传入当前工作目录 g_cwd（绝对路径），使 ls 列出当前目录而非总是根目录。 */
        fs_request(FS_MSG_LIST, g_cwd);
        fs_wait_and_print(FS_MSG_LIST, false);
    } else if (u_strcmp(line, "cat") == 0) {
        if (!*arg) {
            shell_out("usage: cat <FILE>\n");
        } else {
            upcase(arg);
            fs_request(FS_MSG_READ, arg);
            fs_wait_and_print(FS_MSG_READ, true);
        }
    } else if (u_strcmp(line, "reboot") == 0) {
        shell_out("rebooting...\n");
        suki_syscall5(SYS_REBOOT, 0, 0, 0, 0, 0);
    } else if (u_strcmp(line, "poweroff") == 0 || u_strcmp(line, "shutdown") == 0) {
        shell_out("powering off (ACPI S5)...\n");
        suki_syscall5(SYS_REBOOT, 1, 0, 0, 0, 0);   /* mode=1 -> acpi_poweroff */
    } else if (u_strcmp(line, "exec") == 0) {
        if (!*arg) {
            shell_out("usage: exec <FILE> [args...]\n");
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
                shell_out("exec failed: file not found or invalid ELF\n");
            } else {
                uint64_t rc = sys_wait((uint64_t)pid);
                DrainTtyPipe();   /* 先把子进程 stdout/stderr 渲染进终端窗口 */
                shell_out("  [shell] child pid=");
                char db[24];
                shell_out(u_utoa_s((uint64_t)pid, db, sizeof(db)));
                shell_out(" exited (code=");
                shell_out(u_utoa_s(rc, db, sizeof(db)));
                shell_out(")\n");
            }
        }
    } else if (u_strcmp(line, "mkfile") == 0) {
        if (!*arg) { shell_out("usage: mkfile <FILE>\n"); }
        else { upcase(arg); fs_request(FS_MSG_CREATE, arg); fs_wait_status(FS_MSG_CREATE); }
    } else if (u_strcmp(line, "mkdir") == 0) {
        if (!*arg) { shell_out("usage: mkdir <DIR>\n"); }
        else { upcase(arg); fs_request(FS_MSG_MKDIR, arg); fs_wait_status(FS_MSG_MKDIR); }
    } else if (u_strcmp(line, "write") == 0) {
        if (!*arg) { shell_out("usage: write <FILE> <TEXT>\n"); }
        else {
            char *f = arg, *sp = arg;
            while (*sp && *sp != ' ' && *sp != '\t') sp++;
            if (!*sp) { shell_out("usage: write <FILE> <TEXT>\n"); }
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
        if (!*arg) { shell_out("usage: rm <FILE>\n"); }
        else { upcase(arg); fs_request(FS_MSG_UNLINK, arg); fs_wait_status(FS_MSG_UNLINK); }
    } else if (u_strcmp(line, "rename") == 0) {
        if (!*arg) { shell_out("usage: rename <OLD> <NEW>\n"); }
        else {
            char *o = arg, *sp = arg;
            while (*sp && *sp != ' ' && *sp != '\t') sp++;
            if (!*sp) { shell_out("usage: rename <OLD> <NEW>\n"); }
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
        if (!*arg) { shell_out("usage: truncate <FILE> <SIZE>\n"); }
        else {
            char *f = arg, *sp = arg;
            while (*sp && *sp != ' ' && *sp != '\t') sp++;
            if (!*sp) { shell_out("usage: truncate <FILE> <SIZE>\n"); }
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
        shell_out("unknown command: ");
        shell_out(line);
        shell_out("  (try 'help')\n");
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
    if (v && u_strcmp(v, "hello") == 0) pass++; else { fail++; shell_out("[libc-test] FAIL getenv/setenv\n"); }
    setenv("LIBCTEST", "world", 1);
    v = getenv("LIBCTEST");
    if (v && u_strcmp(v, "world") == 0) pass++; else { fail++; shell_out("[libc-test] FAIL setenv overwrite\n"); }
    unsetenv("LIBCTEST");
    if (getenv("LIBCTEST") == NULL) pass++; else { fail++; shell_out("[libc-test] FAIL unsetenv\n"); }

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
    else { fail++; shell_out("[libc-test] FAIL getopt\n"); }

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
    else { fail++; shell_out("[libc-test] FAIL getopt_long\n"); }

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
    if (found_files > 0) pass++; else { fail++; shell_out("[libc-test] FAIL opendir/readdir\n"); }

    /* 5) strerror 字符串非空 */
    if (strerror(ENOENT) && u_strlen(strerror(ENOENT)) > 0) pass++;
    else { fail++; shell_out("[libc-test] FAIL strerror\n"); }

    /* 6) expand_vars：$VAR 与 $? 展开 */
    setenv("GREET", "hi", 1);
    g_last_exit = 7;
    static char ev[128];
    expand_vars(ev, sizeof(ev), "x=$GREET y=$?");
    if (u_strcmp(ev, "x=hi y=7") == 0) pass++;
    else { fail++; shell_out("[libc-test] FAIL expand_vars ("); shell_out(ev); shell_out(")\n"); }
    unsetenv("GREET");

    /* 7) 时间：日历换算 gmtime_r / strftime / mktime 往返（纯 UTC，user/lib/time.c） */
    {
        /* 已知量：2021-01-01T00:00:00Z = 1609459200（周五，年内第 0 天） */
        time_t t0 = (time_t)1609459200LL;
        struct tm tm;
        char tb[32];
        gmtime_r(&t0, &tm);
        size_t sn = strftime(tb, sizeof(tb), "%Y-%m-%dT%H:%M:%S", &tm);
        if (sn > 0 && u_strcmp(tb, "2021-01-01T00:00:00") == 0 &&
            tm.tm_wday == 5 && tm.tm_yday == 0 && tm.tm_mon == 0) {
            time_t back = mktime(&tm);
            if (back == t0) pass++;
            else { fail++; shell_out("[libc-test] FAIL mktime roundtrip\n"); }
        } else {
            fail++; shell_out("[libc-test] FAIL gmtime_r/strftime ("); shell_out(tb); shell_out(")\n");
        }
        /* gettimeofday/clock_gettime 可用性（仅校验调用成功，不假定 RTC 已设） */
        struct timeval tv0;
        struct timespec ts0;
        if (gettimeofday(&tv0, NULL) == 0 && clock_gettime(CLOCK_MONOTONIC, &ts0) == 0) pass++;
        else { fail++; shell_out("[libc-test] FAIL gettimeofday/clock_gettime\n"); }
    }

    /* 8) zlib（lib/zlib @ v1.3.1，submodule）：压缩/解压往返一致性 */
    {
        static const char mmsg[] =
            "SukiOS zlib round trip: The quick brown fox jumps "
            "over the lazy dog. 0123456789 0123456789 0123456789";
        static uint8_t comp[512];
        static uint8_t decomp[512];
        uLongf clen = (uLongf)sizeof(comp);
        uLongf dlen = (uLongf)sizeof(decomp);
        uLong src_len = (uLong)(u_strlen(mmsg) + 1);
        int rc = compress2(comp, &clen, (const Bytef *)mmsg,
                           src_len, Z_DEFAULT_COMPRESSION);
        if (rc == Z_OK && clen > 0 && clen < src_len) {
            rc = uncompress(decomp, &dlen, comp, clen);
            if (rc == Z_OK && dlen == src_len &&
                u_strcmp((char *)decomp, mmsg) == 0) {
                pass++;
                shell_out("[libc-test] zlib roundtrip OK (");
                shell_out(u_utoa_s((uint64_t)src_len, nbuf, sizeof(nbuf)));
                shell_out(" -> ");
                shell_out(u_utoa_s((uint64_t)clen, nbuf, sizeof(nbuf)));
                shell_out(" bytes)\n");
            } else {
                fail++; shell_out("[libc-test] FAIL uncompress\n");
            }
        } else {
            fail++; shell_out("[libc-test] FAIL compress2\n");
        }
    }

    /* 汇总 */
    shell_out("[libc-test] PASS=");
    shell_out(u_utoa_s((uint64_t)pass, nbuf, sizeof(nbuf)));
    shell_out(" FAIL=");
    shell_out(u_utoa_s((uint64_t)fail, nbuf, sizeof(nbuf)));
    shell_out(fail == 0 ? "  ALL OK\n" : "  SOME FAILED\n");
}

/* ===========================================================================
 * 终端仿真（最小 ANSI）：shell 输出经 term_puts 写入字符网格，再由
 * term_render 光栅化为窗口离屏缓冲。仅实现 shell 实际发出的转义
 * （CHA/EL/CUU/CUD/Home/ClearScreen/回车/退格/Tab），足以驱动行编辑。
 * 输出 CSI 解析用独立的 t_esc/t_esc_num，与键盘 ESC 状态机(g_esc)隔离。
 * ========================================================================= */
static void term_scroll(void)
{
    for (int r = 0; r + 1 < TERM_ROWS; r++)
        memcpy(g_tgrid[r], g_tgrid[r+1], TERM_COLS);
    memset(g_tgrid[TERM_ROWS-1], 0, TERM_COLS);
}

static void term_emit_char(char c)
{
    if (!g_win) return;   /* 窗口未创建（启动/自检期）不写网格，避免污染终端缓冲 */
    static int t_esc = 0;       /* 0=普通,1=ESC,2=ESC[ */
    static int t_esc_num = 0;
    if (t_esc == 1) {
        if (c == '[') { t_esc = 2; t_esc_num = 0; return; }
        t_esc = 0; return;       /* 裸 ESC：丢弃 */
    }
    if (t_esc == 2) {
        if (c == 0x1B) { t_esc = 1; t_esc_num = 0; return; }
        if (c >= '0' && c <= '9') { t_esc_num = t_esc_num*10 + (c-'0'); return; }
        if (c == ';') return;
        t_esc = 0;
        int n = t_esc_num >= 1 ? t_esc_num : 1;
        switch (c) {
            case 'G': g_tcx = (t_esc_num>=1 ? t_esc_num-1 : 0); if (g_tcx>=TERM_COLS) g_tcx=TERM_COLS-1; break;
            case 'H': case 'f': g_tcx = 0; g_tcy = 0; break;
            case 'K': for (int x = g_tcx; x < TERM_COLS; x++) g_tgrid[g_tcy][x] = 0; break;
            case 'J': if (n==2){ for (int r=0;r<TERM_ROWS;r++) memset(g_tgrid[r],0,TERM_COLS); g_tcx=0; g_tcy=0; } break;
            case 'D': g_tcx = g_tcx >= n ? g_tcx - n : 0; break;
            case 'C': g_tcx += n; if (g_tcx >= TERM_COLS) g_tcx = TERM_COLS-1; break;
            default: break;
        }
        g_scr_x = g_tcx;
        return;
    }
    if (c == 0x1B) { t_esc = 1; return; }
    if (c == '\n') { g_tcy++; if (g_tcy >= TERM_ROWS) { term_scroll(); g_tcy = TERM_ROWS-1; } g_tcx = 0; g_scr_x = 0; return; }
    if (c == '\r') { g_tcx = 0; g_scr_x = 0; return; }
    if (c == '\b') { if (g_tcx > 0) g_tcx--; g_scr_x = g_tcx; return; }
    if (c == '\t') { g_tcx += 8 - (g_tcx % 8); if (g_tcx >= TERM_COLS) g_tcx = TERM_COLS-1; g_scr_x = g_tcx; return; }
    if (c < 32) return;
    if (g_tcx >= TERM_COLS) { g_tcx = 0; g_tcy++; if (g_tcy >= TERM_ROWS) { term_scroll(); g_tcy = TERM_ROWS-1; } }
    g_tgrid[g_tcy][g_tcx] = c; g_tcx++; g_scr_x = g_tcx;
}

static void term_puts(const char *s)
{
    for (; s && *s; s++) term_emit_char(*s);
    g_term_dirty = true;
}

/* 双写输出：写入终端网格（窗口显示，需窗口已创建）+ 串口镜像（headless 可观测）。
 * 窗口创建前（g_win==NULL）term_emit_char 直接返回，故自检/启动日志不会污染网格，
 * 仅经 u_print 落 serial；窗口创建后所有命令/交互输出同时可见。 */
static void shell_out(const char *s)
{
    term_puts(s);
    u_print(s);
}
static void shell_outn(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) term_emit_char(s[i]);
    g_term_dirty = true;
    u_printn(s, n);
}

/* 把内核「用户 TTY 环形管道」中累积的子进程 stdout/stderr 取回并渲染进本终端窗口
 * （shell 作为终端：普通程序写 fd 1/2 经内核 TTY 后端捕获进该管道，详见
 * kernel/console.c / include/kernel/console.h）。仅渲染到网格（不额外写串口，
 * 避免与 tty_write 的串口镜像重复刷屏）。无数据时立即返回。 */
static void DrainTtyPipe(void)
{
    char buf[256];
    for (;;) {
        long n = sys_tty_read(buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = '\0';
        term_puts(buf);          /* 渲染到终端网格（g_term_dirty 置位） */
    }
}

static void term_render(void)
{
    if (!g_win) return;
    uint32_t *fb = (uint32_t *)SukiGetBuffer(g_win);
    if (!fb) return;
    int W = g_win->w, H = g_win->h;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            fb[y*W + x] = 0x002b2b30;   /* 终端背景 */
    /* 顶部 20px 被 WM 标题栏覆盖，字符从 y=20 起绘制 */
    for (int r = 0; r < TERM_ROWS && (r+1)*8 + 20 <= H; r++) {
        for (int cidx = 0; cidx < TERM_COLS && (cidx+1)*8 <= W; cidx++) {
            char ch = g_tgrid[r][cidx];
            if (!ch) continue;
            int px = cidx*8, py = r*8 + 20;
            const uint8_t *g = font8x8_basic[(uint8_t)ch];
            for (int ry = 0; ry < 8; ry++)
                for (int cx = 0; cx < 8; cx++)
                    if (g[ry] & (1u << cx))
                        fb[(py+ry)*W + (px+cx)] = 0x00e0e0e0;  /* 浅灰前景 */
        }
    }
    SukiFlush(g_win, 0, 0, W, H);
}

/* 键盘字符处理（从 WM 焦点窗口事件端口收到 KEY_DOWN 后调用）。
 * 逻辑与原阻塞主循环一致，仅把回显输出改为 term_puts（窗口 + serial）。 */
static void handle_key(char c)
{
    if (g_esc == ESC_ESC) {
        if (c == '[') { g_esc = ESC_BRK; g_esc_num = 0; return; }
        g_esc = ESC_NONE; return;
    }
    if (g_esc == ESC_BRK) {
        if (c == 0x1B) { g_esc = ESC_ESC; g_esc_num = 0; return; }
        if (c >= '0' && c <= '9') { g_esc_num = g_esc_num*10 + (c-'0'); return; }
        if (c == '~') { if (g_esc_num == 3) edit_delete(); g_esc = ESC_NONE; g_esc_num = 0; return; }
        int code = c; g_esc = ESC_NONE; g_esc_num = 0;
        switch (code) {
            case 'A': if (g_hist_count>0 && g_hist_idx>0){g_hist_idx--; line_replace(g_hist[g_hist_idx]);} break;
            case 'B': if (g_hist_count>0 && g_hist_idx<g_hist_count){g_hist_idx++; if(g_hist_idx<g_hist_count) line_replace(g_hist[g_hist_idx]); else line_replace("");} break;
            case 'C': edit_right(); break;
            case 'D': edit_left(); break;
            case 'H': edit_home(); break;
            case 'F': edit_end(); break;
            default: break;
        }
        return;
    }
    if (c == 0x1B) { g_esc = ESC_ESC; return; }
    if (c == '\b' || c == 0x7F) { edit_backspace(); return; }
    if (c == '\t') { edit_tab_complete(); return; }
    if (c == '\n' || c == '\r') {
        term_puts("\n");
        g_line[g_len] = '\0';
        if (g_len > 0) { hist_push(g_line); g_hist_idx = g_hist_count; run_command(g_line); }
        g_len = 0; g_cur = 0; g_line[0] = 0;
        prompt();
        return;
    }
    if (c >= 32 && c < 127) { edit_insert((char)c); }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* 初始化家目录环境变量（bash 风格） */
    setenv("HOME", "/", 1);
    setenv("SHELL", "/BIN/SHELL.SKA", 1);
    setenv("PATH", "/BIN", 1);

    shell_out("\n[shell] SukiOS shell online (Ring3, bash-like)\n");
    libc_selftest();
    sys_port_claim(SHELL_PORT);    /* 仍认领 SHELL_PORT：接收 FS_SERVER 应答（命令执行同步等待） */

    /* shell 作为 WM 管理的窗口程序：创建窗口 + 事件端口，并主动请求焦点。
     * 之后键盘由 WM 转发到本窗口（焦点模型）；close 按钮触发 SUKI_EVENT_WINDOW_CLOSE。 */
    g_win = SukiCreateWindow("Shell", 40, 60, 660, 380, SUKI_WS_DEFAULT);
    if (g_win) {
        g_ep = sys_port_alloc();
        if (g_ep) {
            sys_port_claim(g_ep);
            SukiSetEventPort(g_win, g_ep);
        }
        SukiSetFocus(g_win);
        shell_out("[shell] windowed mode: window id=");
        char b[16]; shell_out(u_utoa_s(g_win->id, b, sizeof(b)));
        shell_out("\n");
    } else {
        shell_out("[shell] warn: window creation failed, serial-only fallback\n");
    }

    /* 后台拉起常驻字体服务（FreeType 渲染），供 pchfnt 等程序调用 */
    shell_out("[shell] boot: launching font service + selftest\n");
    {
        char *fargv[] = { (char*)"/BIN/FONTSRV.SKA", NULL };
        int fpid = sys_task_spawn((char*)"/BIN/FONTSRV.SKA", fargv, NULL);
        if (fpid < 0)
            shell_out("[shell] warn: fontsrv spawn failed\n");
        else
            shell_out("[shell] font service spawned\n");

        /* 启动自检：用 pchfnt 渲染一行文本，验证「加载字体→光栅化→合成」链路
         * （headless 下 fontsrv 降级仅输出统计，仍证明端到端可用） */
        char *pargv[] = {
            (char*)"pchfnt",
            (char*)"--font", (char*)"/FONTS/RESOURCEHANROUNDEDCN-MEDIUM.TTF",
            (char*)"--text", (char*)"SukiOS FreeType",
            (char*)"--size", (char*)"40",
            (char*)"--x",    (char*)"80",
            (char*)"--y",    (char*)"80",
            NULL
        };
        int tpid = sys_task_spawn((char*)"/BIN/PCHFNT.SKA", pargv, NULL);
        if (tpid < 0)
            shell_out("[shell] warn: pchfnt selftest spawn failed\n");
        else
            shell_out("[shell] pchfnt selftest spawned\n");
    }

    /* 注：nettest（网络子系统端到端验证）已由 kmain 作为内嵌 USER_PROG 在开机自检
     * 阶段确定性 spawn（输出经串口落盘，详见 kmain.c）。此处不再重复拉起，避免双
     * 实例造成 socket fd 清理与日志混乱。 */

    shell_out("Type 'help' for commands.\n\n");
    g_len = 0; g_cur = 0; g_line[0] = 0; g_hist_idx = g_hist_count;
    prompt();

    /* shell 作为 WM 管理的窗口程序：键盘经 WM 转发至本窗口事件端口（焦点模型），
     * 输出渲染到窗口离屏缓冲（同时镜像 serial 供 headless 观测）。 */
    for (;;) {
        if (g_win && g_ep) {
            suki_event_t ev;
            while (SukiPollEvent(g_win, &ev)) {
                if (ev.type == SUKI_EVENT_KEY_DOWN)
                    handle_key((char)(unsigned char)ev.u.key.keycode);
                else if (ev.type == SUKI_EVENT_WINDOW_CLOSE) {
                    shell_out("[shell] window close requested, exiting shell\n");
                    if (g_win) { SukiDestroyWindow(g_win); g_win = NULL; }
                    sys_exit(0);
                }
            }
        }
        if (g_term_dirty) { term_render(); g_term_dirty = false; }
        DrainTtyPipe();   /* 持续取回后台/子进程经 TTY 写出的文本并渲染 */
        sys_yield();
    }
    return 0;
}
