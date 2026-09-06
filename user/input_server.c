/*
 * user/input_server.c
 * -----------------------------------------------------------------------------
 * INPUT_SERVER：Ring3 键盘输入服务（手册 6.3）。
 *
 * 内核 IRQ1 只采集原始扫描码；本服务通过 sys_input_read 拉取扫描码，
 * 在用户态完成 US 布局解析（make/break、Shift、CapsLock），再把 ASCII
 * 字符逐个经 mach_msg 发送到 SHELL_PORT（msgh_id = 1 表示键盘字符）。
 *
 * 调用关系：kbd IRQ(内核, 仅采集) -> sys_input_read -> 本服务解析
 *           -> mach_msg -> SHELL_PORT -> shell。
 */
#include "lib/suki.h"
#include "lib/gui_ipc.h"   /* KEY_MSG_DOWN：键盘经 WM 转发到焦点窗口 */

#define MSG_ID_KEYCHAR 100   /* 避免与 FS_MSG_*(1/2) 冲突 */

static void send_char(char c);   /* 前向声明：poll_serial_input 复用 */

/*
 * 串口控制台输入源（headless QEMU 经 -serial 注入，物理部署经 COM1 控制台）。
 * 串口侧已直接给出 ASCII 字符，无需扫描码解析：经内核 SYS_SERIAL_READ 系统
 * 调用读取（用户态不能直访问 IO 端口），把字符当作与键盘同等的输入事件发给
 * shell（回车 \r 规范为 \n）。真实 PS/2 键盘路径完全保留，二者并存。
 */
static void poll_serial_input(void)
{
    long ch;
    while ((ch = sys_serial_read()) >= 0) {
        char c = (char)(unsigned char)ch;
        if (c == '\r') {
            c = '\n';
        }
        send_char(c);
    }
}

static const char t_norm[128] = {
    0,   27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,   'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,   '\\','z','x','c','v','b','n','m',',','.','/',   0,
    '*', 0,  ' ',
};

static const char t_shift[128] = {
    0,   27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,   'A','S','D','F','G','H','J','K','L',':','"','~',
    0,   '|','Z','X','C','V','B','N','M','<','>','?',   0,
    '*', 0,  ' ',
};

/* 扩展键：e0 前缀后的第二字节 -> ANSI 转义序列（发往 shell 逐字节解析）。
 * 注意 PS/2 set1 扫描码：Up=0x48 Down=0x50 Left=0x4B Right=0x4D
 *                       Home=0x47 End=0x4F Delete=0x53 */
static const char *e0_seq(uint8_t c2)
{
    switch (c2) {
        case 0x48: return "\x1b[A";   /* Up    */
        case 0x50: return "\x1b[B";   /* Down  */
        case 0x4B: return "\x1b[D";   /* Left  */
        case 0x4D: return "\x1b[C";   /* Right */
        case 0x47: return "\x1b[H";   /* Home  */
        case 0x4F: return "\x1b[F";   /* End   */
        case 0x53: return "\x1b[3~";  /* Delete */
        default:   return NULL;
    }
}

/* 把键盘字符经 WM 转发给【焦点窗口】（统一输入焦点模型）。
 * 键盘布局解析仍由本服务完成，这里携带已解析 ASCII；WM 据此构造
 * SUKI_EVENT_KEY_DOWN 推送给当前焦点窗口（shell 等窗口程序自行收取）。 */
static void send_char(char c)
{
    struct {
        mach_msg_header_t h;
        uint8_t  ascii;
        uint8_t  modifiers;
        uint8_t  _pad[2];
    } msg;
    msg.h.msgh_bits = 0;
    msg.h.msgh_size = sizeof(msg);
    msg.h.msgh_remote_port = DISPLAY_PORT;   /* WM（DISPLAY_PORT 与鼠标事件同端口） */
    msg.h.msgh_local_port = INPUT_PORT;
    msg.h.msgh_id = KEY_MSG_DOWN;
    msg.h.msgh_reserved = 0;
    msg.ascii = (uint8_t)(unsigned char)c;
    msg.modifiers = 0;
    mach_msg_send(&msg, sizeof(msg));
}

/* 发送一个多字节转义序列（逐字节发，shell 端状态机会重新拼装） */
static void send_escape(const char *s)
{
    for (; *s; s++) send_char(*s);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    bool shift = false, caps = false;
    bool e0 = false;                 /* 是否处于 e0 扩展前缀 */

    u_print("[input] INPUT_SERVER online (Ring3 scancode parser)\n");
    sys_port_claim(INPUT_PORT);    /* A2 项：认领输入接收端口 */

    for (;;) {
        poll_serial_input();           /* 串口控制台输入（headless 测试通道） */
        uint64_t v = suki_syscall5(SYS_INPUT_READ, 0, 0, 0, 0, 0);
        if (v == (uint64_t)-1) {
            sys_yield();               /* 无输入：让出 CPU */
            continue;
        }
        uint8_t sc = (uint8_t)v;

        if (sc == 0xE0) {              /* 扩展键前缀：下一字节决定方向键等 */
            e0 = true;
            continue;
        }
        if (e0) {
            e0 = false;
            if (sc & 0x80) continue;   /* e0 扩展键的 break 帧忽略 */
            const char *seq = e0_seq(sc);
            if (seq) send_escape(seq); /* 把方向键/Tab 等编码为转义序列发给 shell */
            continue;
        }

        if (sc & 0x80) {               /* break（松开） */
            uint8_t code = sc & 0x7F;
            if (code == 0x2A || code == 0x36) {
                shift = false;
            }
            continue;
        }
        if (sc == 0x2A || sc == 0x36) {
            shift = true;
            continue;
        }
        if (sc == 0x3A) {
            caps = !caps;
            continue;
        }
        if (sc >= 128) {
            continue;
        }
        char c = shift ? t_shift[sc] : t_norm[sc];
        if (!c) {
            continue;
        }
        if (caps && c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        } else if (caps && shift && c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        send_char(c);
    }
    return 0;
}
