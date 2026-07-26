/*
 * kernel/arch/x86_64/keyboard.c
 * -----------------------------------------------------------------------------
 * PS/2 键盘中断驱动 (IRQ1)。读取 0x60 端口扫描码，最小 US 布局解析后放入
 * 环形缓冲，供早期内核 Shell 轮询读取。
 *
 * 调用关系：kmain() -> keyboard_init(); IRQ1 -> kbd_irq_handler()。
 */
#include <kernel/keyboard.h>
#include <kernel/interrupts.h>
#include <kernel/pic.h>
#include <kernel/io.h>
#include <kernel/console.h>

#define KBD_DATA   0x60

/* US 扫描码集 1 -> ASCII（非移位），仅覆盖常用键 */
static const char g_scancode_ascii[128] = {
    0,   27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,   'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,   '\\','z','x','c','v','b','n','m',',','.','/',   0,
    '*', 0,  ' ',
};

static const char g_scancode_shift[128] = {
    0,   27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,   'A','S','D','F','G','H','J','K','L',':','"','~',
    0,   '|','Z','X','C','V','B','N','M','<','>','?',   0,
    '*', 0,  ' ',
};

/* ASCII 环形缓冲（内核救援终端用） */
#define KBD_BUF_SIZE 256
static volatile char g_buf[KBD_BUF_SIZE];
static volatile uint32_t g_head = 0, g_tail = 0;

/* 原始扫描码环形缓冲（用户态 INPUT_SERVER 经 sys_input_read 消费） */
static volatile uint8_t g_raw[KBD_BUF_SIZE];
static volatile uint32_t g_rhead = 0, g_rtail = 0;

static bool g_shift = false;
static bool g_caps  = false;

static void kbd_push(char c)
{
    uint32_t next = (g_head + 1) % KBD_BUF_SIZE;
    if (next != g_tail) {          /* 未满 */
        g_buf[g_head] = c;
        g_head = next;
    }
}

static void kbd_irq_handler(registers_t *r)
{
    (void)r;
    uint8_t sc = inb(KBD_DATA);

    /* 红线（手册 6.3）：内核只采集原始扫描码；解析由 Ring3 INPUT_SERVER
     * 完成。以下 ASCII 解析仅作为救援终端的后备路径保留。 */
    uint32_t rnext = (g_rhead + 1) % KBD_BUF_SIZE;
    if (rnext != g_rtail) {
        g_raw[g_rhead] = sc;
        g_rhead = rnext;
    }

    if (sc & 0x80) {
        /* break code（松开） */
        uint8_t code = sc & 0x7F;
        if (code == 0x2A || code == 0x36) {
            g_shift = false;
        }
        return;
    }

    /* make code（按下） */
    if (sc == 0x2A || sc == 0x36) {     /* 左/右 Shift */
        g_shift = true;
        return;
    }
    if (sc == 0x3A) {                    /* Caps Lock */
        g_caps = !g_caps;
        return;
    }
    if (sc >= 128) {
        return;
    }

    char c = g_shift ? g_scancode_shift[sc] : g_scancode_ascii[sc];
    if (c == 0) {
        return;
    }
    /* Caps Lock 仅影响字母 */
    if (g_caps && c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    } else if (g_caps && g_shift && c >= 'A' && c <= 'Z') {
        c = (char)(c - 'A' + 'a');
    }
    kbd_push(c);
}

void keyboard_init(void)
{
    g_head = g_tail = 0;
    register_interrupt_handler(IRQ1, kbd_irq_handler);
    pic_clear_mask(1);                  /* 放开 IRQ1 */
    kprintf("[kbd] PS/2 keyboard ready (IRQ1)\n");
}

char keyboard_getchar(void)
{
    if (g_tail == g_head) {
        return 0;                       /* 空 */
    }
    char c = g_buf[g_tail];
    g_tail = (g_tail + 1) % KBD_BUF_SIZE;
    return c;
}

/* 取一个原始扫描码；无数据返回 -1（sys_input_read 底层） */
int keyboard_get_scancode(void)
{
    if (g_rtail == g_rhead) {
        return -1;
    }
    uint8_t sc = g_raw[g_rtail];
    g_rtail = (g_rtail + 1) % KBD_BUF_SIZE;
    return (int)sc;
}
