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
#include <kernel/ioapic.h>
#include <kernel/io.h>
#include <kernel/console.h>

#define KBD_DATA   0x60
#define KBD_CMD    0x64
#define KBD_STATUS 0x64

/* status 寄存器位 */
#define STS_OUT_FULL 0x01   /* 输出缓冲满（可读 0x60） */
#define STS_IN_FULL  0x02   /* 输入缓冲满（写 0x60/0x64 前须清） */
#define STS_AUX      0x20   /* 数据来自辅助设备(鼠标) */

/* 控制器命令 */
#define CC_READ_CFG  0x20   /* 读配置字节 */
#define CC_WRITE_CFG 0x60   /* 写配置字节 */
#define CC_SELFTEST  0xAA   /* 控制器自检，期望 0x55 */
#define CC_TEST_P1   0xAB   /* 端口1接口测试，期望 0x00 */
#define CC_DISABLE_P1 0xAD  /* 禁用端口1 */
#define CC_ENABLE_P1 0xAE   /* 使能端口1 */
#define CC_DISABLE_P2 0xA7  /* 禁用端口2 */
#define CC_ENABLE_P2  0xA8  /* 使能端口2 */

/* 设备(键盘)命令 */
#define KCMD_ENABLE_SCAN 0xF4   /* 使能扫描，期望 ACK 0xFA */
#define KCMD_RESET       0xFF   /* 复位，期望 0xFA 后 0xAA */
#define KBD_ACK 0xFA

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

/* 等待输入缓冲空（写前）。带超时，避免死锁（红线：不信任硬件响应）。 */
static bool kbd_wait_input(void)
{
    for (int i = 0; i < 100000; i++) {
        if (!(inb(KBD_STATUS) & STS_IN_FULL)) {
            return true;
        }
        io_wait();
    }
    return false;
}

/* 等待输出缓冲满（读前）。带超时。 */
static bool kbd_wait_output(void)
{
    for (int i = 0; i < 100000; i++) {
        if (inb(KBD_STATUS) & STS_OUT_FULL) {
            return true;
        }
        io_wait();
    }
    return false;
}

/* 发控制器命令并读一个响应字节（带超时，返回 false 表示无响应）。 */
static bool kbd_cmd_response(uint8_t cmd, uint8_t *resp)
{
    if (!kbd_wait_input()) {
        return false;
    }
    outb(KBD_CMD, cmd);
    if (!kbd_wait_output()) {
        return false;
    }
    *resp = inb(KBD_DATA);
    return true;
}

/* 发控制器写参数（命令后第二个字节）。 */
static bool kbd_write_param(uint8_t param)
{
    if (!kbd_wait_input()) {
        return false;
    }
    outb(KBD_DATA, param);
    return true;
}

/* 发键盘设备命令（经 0x60 数据端口），返回 ACK/响应字节。 */
static bool kbd_dev_cmd(uint8_t cmd, uint8_t *resp)
{
    if (!kbd_wait_input()) {
        return false;
    }
    outb(KBD_DATA, cmd);
    if (!kbd_wait_output()) {
        return false;
    }
    *resp = inb(KBD_DATA);
    return true;
}

/*
 * 完整 8042 控制器初始化（OSDev《I8042 PS/2 Controller》"Initialising the
 * PS/2 Controller" 步骤 3~9）。GRUB/SeaBIOS 引导时固件已做过一遍，但 PVH /
 * UEFI 等无 BIOS 路径下内核必须自建，否则键盘端口未使能、按键不产生 IRQ。
 * 重复初始化对 SeaBIOS 路径无害（控制器已就绪则自检/测试直接通过）。
 */
static void kbd_controller_init(void)
{
    uint8_t resp;

    /* 步骤3：禁用两个端口 */
    kbd_wait_input(); outb(KBD_CMD, CC_DISABLE_P1);
    kbd_wait_input(); outb(KBD_CMD, CC_DISABLE_P2);

    /* 步骤4：冲刷输出缓冲（丢弃残留字节） */
    while (inb(KBD_STATUS) & STS_OUT_FULL) {
        inb(KBD_DATA);
        io_wait();
    }

    /* 步骤5：BYTE0 配置字节。读-改-写：使能端口1(位0)、端口2(位1)、
     * 端口1中断(位0)、端口2中断(位1)；保留其余位。 */
    uint8_t cfg = 0;
    if (kbd_cmd_response(CC_READ_CFG, &cfg)) {
        cfg |= 0x03;          /* 使能端口1+端口2及其中断 */
        cfg &= ~0x04;         /* 保留 System Flag 不变由固件设 */
        kbd_wait_input(); outb(KBD_CMD, CC_WRITE_CFG);
        kbd_write_param(cfg);
    }

    /* 步骤6：控制器自检 */
    if (kbd_cmd_response(CC_SELFTEST, &resp)) {
        if (resp != 0x55) {
            kprintf("[kbd] WARN: controller self-test = 0x%02x (expected 0x55)\n", resp);
        }
    }

    /* 步骤7/8：端口1接口测试（非双通道可跳过，简单处理） */
    if (kbd_cmd_response(CC_TEST_P1, &resp)) {
        if (resp != 0x00) {
            kprintf("[kbd] WARN: port1 interface test = 0x%02x (expected 0x00)\n", resp);
        }
    }

    /* 步骤9：使能端口1 */
    kbd_wait_input(); outb(KBD_CMD, CC_ENABLE_P1);

    /* 键盘设备：复位并重新使能扫描（防御性；部分固件/模拟器需显式 enable） */
    if (kbd_dev_cmd(KCMD_RESET, &resp)) {
        /* 复位后期望 0xFA(ACK) 随后 0xAA(自测通过)；这里仅消耗 ACK，
         * 再等可能的 0xAA。 */
        if (resp == KBD_ACK) {
            kbd_wait_output();
            inb(KBD_DATA);    /* 吞掉 0xAA */
        }
    }
    /* OSDev《PS/2 Keyboard》"Set Scan Code Set"：SeaBIOS/QEMU 默认键盘为扫描
     * 码集 2，而本内核后备 ASCII 表（g_scancode_ascii）按扫描码集 1 解释。
     * 显式把键盘设为集 1（设备命令 0xF0 + 0x01），使内核表与 Ring3 INPUT_SERVER
     * 拿到的扫描码语义一致；不同固件默认集下也不再错位。 */
    uint8_t setresp = 0;
    if (kbd_dev_cmd(0xF0, &setresp) && setresp == KBD_ACK) {
        kbd_dev_cmd(0x01, &setresp);   /* 选集 1 */
    }
    /* 重新使能扫描（reset 后扫描默认开启，但再发一次确保，忽略 resend） */
    uint8_t ack = 0;
    for (int retry = 0; retry < 3; retry++) {
        if (kbd_dev_cmd(KCMD_ENABLE_SCAN, &ack)) {
            if (ack == KBD_ACK) {
                break;
            }
            if (ack == 0xFE) {   /* resend */
                continue;
            }
        }
        break;
    }
}

void keyboard_init(void)
{
    g_head = g_tail = 0;
    g_rhead = g_rtail = 0;
    kbd_controller_init();
    register_interrupt_handler(IRQ1, kbd_irq_handler);
    /* P0-2：键盘走 I/O APIC（GSI1 -> 向量 IRQ1）。边沿触发、高电平有效，
     * 目标为 BSP 的 LAPIC（QEMU 下单核 LAPIC ID=0）。见 kmain 的 ioapic_init。 */
    ioapic_route(1, IRQ1, false, false, 0);
    kprintf("[kbd] PS/2 keyboard ready (IOAPIC GSI1 -> IRQ1)\n");
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
