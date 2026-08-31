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

/*
 * 喂一个键盘字节（扫描码，集1）给内核键盘状态机：写入原始缓冲供 Ring3
 * INPUT_SERVER 取用，并维护救援终端的 ASCII 后备解析（shift/caps/break）。
 * 由 kbd_irq_handler 与 mouse_irq_handler（统一 PS/2 分发）共用，确保键盘
 * 数据无论经 IRQ1 还是误经 IRQ12 都能正确送达，绝不被鼠标 handler 吞掉。
 */
void kbd_feed_byte(uint8_t sc)
{
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

/* 鼠标字节经统一分发转交键盘状态机（由 mouse_irq_handler 在 STS_AUX=0 时调用）。
 * 声明在此处以避免头文件循环依赖；实现位于 mouse.c。 */
extern void mouse_feed_byte(uint8_t b);

/*
 * IRQ1 处理程序（键盘）。
 * OSDev《8042 PS/2 Controller》"Keyboard/Auxiliary Device Data"：状态寄存器 STS_AUX
 * (bit5) 标记输出缓冲中的数据来源——置位=辅助设备(鼠标)，清零=主设备(键盘)。
 * 在部分固件/模拟器下，鼠标数据落到输出缓冲时可能同时拉 IRQ1（与 IRQ12 并存），
 * 此时若不加 STS_AUX 检查直接读 0x60，会把鼠标包首字节当扫描码解析成乱码字符
 * （现象：移动鼠标在屏幕上产生键盘字符、光标不动）。故此处统一按 STS_AUX 分流：
 *   非 AUX -> kbd_feed_byte (键盘)；AUX -> mouse_feed_byte (鼠标，绝不消费键盘路径)。
 * 每个字节只被读一次（inb 清 STS_OUT_FULL），后续另一个 IRQ handler 跑时缓冲已空
 * 自然 return，不会重复消费。
 */
static void kbd_irq_handler(registers_t *r)
{
    (void)r;
    uint8_t st = inb(KBD_STATUS);
    if (!(st & STS_OUT_FULL)) {
        return;                 /* 无数据（spurious，忽略） */
    }
    uint8_t sc = inb(KBD_DATA);
    if (st & STS_AUX) {
        /* 数据来自鼠标：转交鼠标状态机，绝不吞掉（统一 PS/2 分发） */
        mouse_feed_byte(sc);
    } else {
        kbd_feed_byte(sc);
    }
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
     * 端口1中断(位0)、端口2中断(位1)；保留其余位。
     * 关键：必须「保留 / 显式开启」翻译模式位(位6)。
     *   OSDev《8042 PS/2 Controller》"Translation"：翻译位开启时，8042 把键盘
     *   默认输出的扫描码集 2（QEMU/SeaBIOS 固件默认集）自动翻译成等价的集 1
     *   码值（含 make/break 编码）再交给 CPU。本内核与 Ring3 INPUT_SERVER 的
     *   ASCII 表均按**集 1** 解释（break = 最高位 0x80 置位）。因此保留翻译位
     *   开启，CPU 收到的就是正确的集 1 码值，按键映射一一对应。
     *   历史 bug：旧代码在此 `cfg &= ~0x40` 关闭翻译位，期望「纯 Set 1 直通」。
     *   但 QEMU 键盘固件默认仍是集 2，关闭翻译后集 2 原始码直通给 CPU，被本
     *   内核当集 1 解析 -> 全面错位（典型现象：按 Ctrl 却打印 'f'）。故此处
     *   改为 `cfg |= 0x40` 确保翻译位开启，而非清除。 */
    uint8_t cfg = 0;
    if (kbd_cmd_response(CC_READ_CFG, &cfg)) {
        cfg |= 0x03;          /* 使能端口1+端口2及其中断 */
        cfg |= 0x40;          /* 开启翻译模式(位6)：集2自动翻译为集1给CPU */
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
        /* 复位后期望 0xFA(ACK) 随后 0xAA(BAT 通过)。SeaBIOS/QEMU 默认扫描
         * 码集为 Set 2；此处把复位握手产生的所有回发字节（0xFA + 0xAA，
         * 可能还有多余字节）全部消费干净，避免残留字节干扰后续命令的 ACK
         * 读取。键盘保持默认 Set 2 —— 由步骤5 开启的翻译位自动翻译为集 1
         * 交给 CPU，本内核据此解析，键位一一对应。 */
        if (resp == KBD_ACK) {
            while (kbd_wait_output()) {   /* 冲刷直到输出缓冲空 */
                inb(KBD_DATA);
            }
        }
    }
    /* 注意：此处不再向键盘发「设置扫描码集」命令。
     *   旧实现发 0xF0 0x01 强制键盘切到 Set 1，本意配合「关闭翻译位」实现纯
     *   Set 1 直通；但 QEMU 键盘固件对 0xF0 0x01 的响应不可靠（命令返回 ACK
     *   而内部仍按 Set 2 输出），导致「关闭翻译位 + 实际 Set 2 输出」组合 ->
     *   集 2 原始码被当集 1 解析 -> 按键全面错位（按 Ctrl 打印 'f' 等）。
     *   现改为「开启翻译位 + 键盘保持默认 Set 2」：翻译器把 Set 2 稳定地翻译
     *   为等价的 Set 1 码值（含 make/break 编码），CPU 收到标准 Set 1，键位
     *   完全正确。这是真实 PC / BIOS 的标准做法，QEMU 亦稳定支持。 */
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
    kprintf("[kbd] PS/2 keyboard configured (translation ON, scan set 2->1)\n");
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
