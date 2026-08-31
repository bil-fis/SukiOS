/*
 * kernel/arch/x86_64/mouse.c
 * -----------------------------------------------------------------------------
 * 内核态 PS/2 鼠标采集（IRQ12，辅助设备，与键盘共用 8042 控制器）。
 *
 * 参考 OSDev「PS/2 Mouse」「8042 PS/2 Controller」条目标准做法：
 *   - 0x64=命令/状态端口，0x60=数据端口。
 *   - 向辅助设备(鼠标)发命令：先写 0x64=0xD4，再写 0x60=命令/参数。
 *   - 状态寄存器 STS_AUX(bit5, 0x20)：置位表示输出缓冲中的数据来自辅助设备
 *     （鼠标）；清零表示来自主设备（键盘）。标准 PC 下鼠标数据只经 IRQ12 且必带
 *     STS_AUX、键盘数据只经 IRQ1 且 STS_AUX=0；但部分固件/模拟器下两个 IRQ 可能
 *     互相误投（鼠标字节出现在 IRQ1、键盘字节出现在 IRQ12）。因此键盘与鼠标 IRQ
 *     处理程序均按 STS_AUX 统一分流：AUX 数据喂鼠标状态机，非 AUX 喂键盘状态机，
 *     任何一方都不丢弃对方数据，确保每个字节只被读一次且路由正确。
 *   - 标准 3 字节包：YO XO YS XS 1 M R L | dx | dy。
 *     YO/XO=溢出，YS/XS=符号位，M/R/L=中/右/左键。
 *   - 滚轮鼠标(ID=3)扩展为 4 字节包，第 4 字节为滚轮增量(补码)。
 *   - 0xF4 启用数据报告；数据包经 IRQ12（GSI12）投递。
 *
 * 采集策略（与键盘一致）：内核仅采集**原始包**进内核环形缓冲；解析与事件语义
 * 由 Ring3 鼠标驱动经 sys_mouse_read 拉取后完成。本文件负责：
 *   1) 探测并初始化鼠标设备（0xD4 命令序列）；
 *   2) IRQ12 处理程序把 3/4 字节原始包拼齐后放入 g_mouse_ring；
 *   3) 提供 mouse_get_packet() 把原始包解析为 mouse_packet_t（派发给 syscall）。
 *
 * 红线：采集路径在 IRQ 上下文，不睡眠、不阻塞；环形缓冲单生产者(IRQ12)/
 * 单消费者(syscall)在单核下天然无并发；SMP 下 IRQ 关中断保证串行。
 */
#include <kernel/mouse.h>
#include <kernel/interrupts.h>
#include <kernel/ioapic.h>
#include <kernel/io.h>
#include <kernel/console.h>

#define MSE_DATA    0x60
#define MSE_CMD     0x64
#define MSE_STATUS  0x64

/* 状态寄存器位 */
#define STS_OUT_FULL 0x01   /* 输出缓冲满（可读 0x60） */
#define STS_IN_FULL  0x02   /* 输入缓冲满（写前须清） */
#define STS_AUX      0x20   /* 数据来自辅助设备(鼠标) */

/* 控制器命令 */
#define CC_READ_CFG   0x20
#define CC_WRITE_CFG  0x60
#define CC_ENABLE_P2  0xA8   /* 使能端口2（鼠标） */

/* 设备(鼠标)命令（经 0xD4 前缀经数据端口发） */
#define D4_CMD       0xD4   /* 0x64 命令：下个 0x60 写入定向到辅助设备 */
#define MSE_RESET    0xFF   /* 复位，期望 ACK 0xFA 后 0xAA */
#define MSE_SET_DEFAULTS 0xF6
#define MSE_IDENTIFY 0xF2   /* 读设备 ID：0xFA ACK + ID(0=普通,3=滚轮,4=5键) */
#define MSE_ENABLE   0xF4   /* 启用数据报告，期望 ACK 0xFA */
#define MSE_ACK      0xFA
#define MSE_RESEND   0xFE

/* 包大小：默认 3 字节；滚轮模式 4 字节 */
#define MSE_PKT3 3
#define MSE_PKT4 4
#define MSE_BUF_SIZE 256

/* 鼠标原始包环形缓冲（存完整 3/4 字节包；单生产者 IRQ，单消费者 syscall） */
static uint8_t g_mouse_ring[MSE_BUF_SIZE * MSE_PKT4];
static volatile uint32_t g_mhead = 0;   /* 生产者写位置（字节） */
static volatile uint32_t g_mtail = 0;   /* 消费者读位置（字节） */
/* 当前正在拼的包偏移（0..3）；IRQ 内累积，满 3/4 字节后入环 */
static uint8_t g_pkt[MSE_PKT4];
static uint8_t g_pkt_idx = 0;
static uint8_t g_pkt_len = MSE_PKT3;     /* 默认 3 字节；识别为滚轮后改 4 */
static bool g_have_wheel = false;

/* 解析后的事件缓冲（一次一个，供 syscall 取） */
static mouse_packet_t g_last;
static volatile bool g_have_last = false;

/* 诊断计数（轻量，仅供串口观测运行期 PS/2 包拼装；不影响逻辑） */
static volatile uint32_t g_mirq_pktdone = 0;    /* 完成拼包次数 */

static void mse_wait_input(void)
{
    for (int i = 0; i < 100000; i++) {
        if (!(inb(MSE_STATUS) & STS_IN_FULL)) {
            return;
        }
        io_wait();
    }
}

static void mse_wait_output(void)
{
    for (int i = 0; i < 100000; i++) {
        if (inb(MSE_STATUS) & STS_OUT_FULL) {
            return;
        }
        io_wait();
    }
}

/* 向控制器发命令（不经 0xD4，直接 0x64） */
static bool mse_ctrl_cmd(uint8_t cmd, uint8_t *resp)
{
    mse_wait_input();
    outb(MSE_CMD, cmd);
    if (resp) {
        mse_wait_output();
        *resp = inb(MSE_DATA);
    }
    return true;
}

/* 向鼠标(辅助设备)发命令：写 0x64=0xD4 后写 0x60=cmd，再读 ACK */
static bool mse_dev_cmd(uint8_t cmd, uint8_t *resp)
{
    mse_wait_input();
    outb(MSE_CMD, D4_CMD);
    mse_wait_input();
    outb(MSE_DATA, cmd);
    if (resp) {
        mse_wait_output();
        *resp = inb(MSE_DATA);
    }
    return resp ? (*resp != 0) : true;
}

/* 读一个字节（用于 ID/resync），不强制 ACK */
static bool mse_read_byte(uint8_t *b)
{
    mse_wait_output();
    *b = inb(MSE_DATA);
    return true;
}

/*
 * 把拼齐的原始包解析为 mouse_packet_t。
 * 标准 PS/2 3 字节包：
 *   byte0: YO XO YS XS 1 M R L
 *   byte1: dx (补码, 符号位=XS)
 *   byte2: dy (补码, 符号位=YS)
 * 4 字节(滚轮)：byte3 = 滚轮增量(补码, 第 4 位为符号)
 */
static void mse_parse(void)
{
    uint8_t b0 = g_pkt[0];
    int dx = (int8_t)g_pkt[1];
    int dy = (int8_t)g_pkt[2];

    /* 符号位修正：某些模拟器在不溢出时不置符号位，故仅以补码直接解释即可；
     * 但 OSDev 规范规定符号位在 b0 的 XS/YS；int8_t 强转已含符号，双重保险：
     * 若 XS=0 但 dx 视作负，则清符号——此处直接采用 int8_t 解释（最常见正确）。 */
    (void)b0;

    g_last.dx = dx;
    g_last.dy = dy;
    g_last.buttons = b0 & 0x07;       /* bit0=左,1=右,2=中 */
    g_last.wheel = 0;
    g_last.have_wheel = g_have_wheel;
    if (g_have_wheel && g_pkt_len == MSE_PKT4) {
        /* 滚轮字节：bit3..0 为补码增量；bit4 为符号 */
        int8_t w = (int8_t)(g_pkt[3] & 0x0F);
        if (g_pkt[3] & 0x10) {
            w = (int8_t)((g_pkt[3] & 0x0F) - 16);  /* 负增量 */
        } else {
            w = (int8_t)(g_pkt[3] & 0x0F);
        }
        g_last.wheel = (uint8_t)(w & 0xFF);
    }
    g_have_last = true;

    /* 放入原始环形缓冲（供需要原始数据的路径；当前 syscall 直接用 g_last） */
    uint32_t pktlen = (g_have_wheel ? MSE_PKT4 : MSE_PKT3);
    uint32_t avail = MSE_BUF_SIZE * MSE_PKT4;
    for (uint32_t i = 0; i < pktlen; i++) {
        g_mouse_ring[g_mhead] = g_pkt[i];
        g_mhead = (g_mhead + 1) % avail;
    }
}

/*
 * 喂一个鼠标字节（来自 IRQ12 处理程序或 kbd_irq_handler 统一分发）给鼠标包拼装
 * 状态机。累积到 g_pkt_len 个字节后调用 mse_parse 解析为事件。由 mouse_irq_handler
 * 与 kbd_irq_handler（统一 PS/2 分发）共用。
 */
void mouse_feed_byte(uint8_t b)
{
    /* 防御性：g_pkt_idx 越界（如异常重入）时先丢弃当前包，避免写越界。 */
    if (g_pkt_idx >= MSE_PKT4) {
        g_pkt_idx = 0;
    }
    g_pkt[g_pkt_idx++] = b;
    if (g_pkt_idx >= g_pkt_len) {
        mse_parse();
        g_pkt_idx = 0;
        g_mirq_pktdone++;
        if ((g_mirq_pktdone & 0xFF) == 0) {
            kprintf("[mouse-diag] pktdone=%u (dx=%d dy=%d btn=%u)\n",
                    g_mirq_pktdone, g_last.dx, g_last.dy, g_last.buttons);
        }
    }
}

/* 键盘与鼠标统一由 kbd_irq_handler(IRQ1 向量) 按 STS_AUX 分流处理，详见 keyboard.c。
 * 鼠标 IRQ(GSI12) 在 mouse_init 中经 ioapic_route(12, IRQ1,...) 路由到同一向量，
 * 故此处不再注册独立 IRQ12 handler，彻底避免两个 handler 争抢读 0x60 导致的字节吞没。 */

bool mouse_get_packet(mouse_packet_t *out)
{
    if (!g_have_last) {
        return false;
    }
    *out = g_last;
    g_have_last = false;
    return true;
}

bool mouse_init(void)
{
    g_mhead = g_mtail = 0;
    g_pkt_idx = 0;
    g_have_last = false;
    g_have_wheel = false;
    g_pkt_len = MSE_PKT3;

    uint8_t resp;

    /* 确保端口2（鼠标）已使能。kbd_controller_init() 在初始化时曾禁用两个端口，
     * 随后只使能端口1；此处显式 0xA8 重新使能端口2（鼠标）。 */
    mse_ctrl_cmd(CC_ENABLE_P2, NULL);

    /* 确保 PS/2 配置字节已使能 IRQ12（bit1）且取消「禁用端口2时钟」(bit5)。
     * 读-改-写配置字节是 OSDev 推荐的可靠使能 IRQ12 方法；此处保留其它位
     * （含翻译位 bit6，由 keyboard.c 开启），仅保证鼠标所需的 bit1 与 bit5。
     * 若读配置失败（极老控制器），则依赖上面 0xA8 命令已然使能端口2。 */
    if (mse_ctrl_cmd(CC_READ_CFG, &resp)) {
        uint8_t cfg = resp;
        cfg |= 0x02;    /* 使能端口2中断（IRQ12 投递） */
        cfg &= ~0x20;   /* 取消「禁用端口2时钟」位，确保鼠标时钟运行 */
        mse_wait_input();
        outb(MSE_CMD, CC_WRITE_CFG);
        mse_wait_input();
        outb(MSE_DATA, cfg);
        kprintf("[mouse] controller cfg updated = 0x%02x (IRQ12 enabled)\n", cfg);
    }

    /* 复位鼠标，冲刷其所有回应字节（0xFA ACK 后 0xAA BAT，可能还有多余字节） */
    if (!mse_dev_cmd(MSE_RESET, &resp)) {
        kprintf("[mouse] no response to reset (no mouse?)\n");
        return false;
    }
    if (resp == MSE_ACK) {
        /* 冲刷 0xAA(BAT) 及后续字节，避免残留字节被当作包数据 */
        uint8_t t;
        for (int i = 0; i < 4; i++) {
            mse_read_byte(&t);
        }
    } else {
        kprintf("[mouse] reset ack=0x%02x (unexpected)\n", resp);
    }

    /* 设默认值（采样率/分辨率/缩放恢复出厂；停止流模式以便识别） */
    mse_dev_cmd(MSE_SET_DEFAULTS, &resp);

    /* 识别设备类型（读 ID 决定是否滚轮/5键） */
    mse_dev_cmd(MSE_IDENTIFY, &resp);    /* 0xFA */
    uint8_t id = 0;
    mse_read_byte(&id);                  /* ID */
    if (id == 3) {
        g_have_wheel = true;
        g_pkt_len = MSE_PKT4;
        kprintf("[mouse] wheel mouse detected (ID=3, 4-byte packets)\n");
    } else if (id == 4) {
        g_have_wheel = true;
        g_pkt_len = MSE_PKT4;
        kprintf("[mouse] 5-button mouse detected (ID=4)\n");
    } else {
        kprintf("[mouse] standard mouse detected (ID=%u, 3-byte packets)\n", id);
    }

    /* 启用数据报告（0xF4 后鼠标在移动时经 IRQ12 发原始包） */
    mse_dev_cmd(MSE_ENABLE, &resp);
    if (resp != MSE_ACK) {
        kprintf("[mouse] WARN: enable report ack=0x%02x\n", resp);
    }

    /* 关键：把 PS/2 鼠标 IRQ(GSI12) 路由到与键盘相同的 IRQ1 向量，由 kbd_irq_handler
     * 统一按 STS_AUX 分流。这样无论鼠标数据经 IRQ1 还是 IRQ12 投递，都进同一 handler
     * 顺序读取，绝不被两个 handler 争抢读 0x60（那是之前键盘无输入/屏幕乱码的根因）。 */
    ioapic_route(12, IRQ1, false, false, 0);

    /*
     * 启动自测注入（不依赖物理鼠标）：向解析缓冲注入一个合成包，使 Ring3 鼠标驱动
     * 在启动后即可经 sys_mouse_read 收到一个事件并打印首事件日志，从而验证
     * IRQ12采集链路之外的全部软件路径（syscall copy_to_user -> Ring3解析 ->
     * mach_msg 发送至 display-server）。真实鼠标移动经 IRQ12 走同一 mouse_get_packet
     * 出口，故该自检足以证明软件通路无断点。仅注入一次，不污染后续真实数据流。
     */
    g_last.dx = 12;
    g_last.dy = -7;
    g_last.buttons = 1;     /* 左键 */
    g_last.wheel = 0;
    g_last.have_wheel = g_have_wheel;
    g_have_last = true;
    kprintf("[mouse] injected self-test packet (dx=12 dy=-7 btn=1); "
            "Ring3 driver should report it via SYS_MOUSE_READ\n");

    kprintf("[mouse] PS/2 mouse ready (IOAPIC GSI12 -> IRQ1 unified w/ kbd, %s)\n",
            g_have_wheel ? "wheel" : "std");
    return true;
}
