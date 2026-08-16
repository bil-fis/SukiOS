/*
 * kernel/arch/x86_64/serial.c
 * -----------------------------------------------------------------------------
 * COM1 (0x3F8) 16550 UART 串口驱动。用于内核早期调试日志（手册 8.1）。
 *
 * 调用关系：kmain() -> serial_init() -> serial_writestr()/serial_write_hex()。
 * panic() 及其它模块也会调用 serial_write* 输出诊断信息。
 */
#include <kernel/serial.h>
#include <kernel/io.h>
#include <kernel/klog.h>     /* P0-R3a：所有串口输出镜像进内存环缓冲 */
#include <kernel/spinlock.h>  /* P0-R3b：多核串行化串口输出，避免 SMP 下字符交错/行丢失 */

#define COM1_BASE   0x3F8

/* SMP 下多核并发写 COM1 会导致字符交错甚至整行丢失（诊断噪音掩盖真实
 * 卡死点），故用自旋锁串行化整个 serial_write。中断上下文亦安全。 */
static spinlock_t g_serial_lock = SPINLOCK_INIT("serial");

/* 16550 寄存器偏移 */
#define UART_DATA        0   /* DLAB=0: 数据寄存器 / DLAB=1: 除数低字节 */
#define UART_IER         1   /* DLAB=0: 中断使能 / DLAB=1: 除数高字节 */
#define UART_FCR         2   /* FIFO 控制 */
#define UART_LCR         3   /* 线路控制 */
#define UART_MCR         4   /* Modem 控制 */
#define UART_LSR         5   /* 线路状态 */

#define LSR_THR_EMPTY    0x20  /* 发送保持寄存器空 */
#define LSR_DATA_READY   0x01  /* 接收数据就绪（DR） */

bool serial_read_ready(void)
{
    return (inb(COM1_BASE + UART_LSR) & LSR_DATA_READY) != 0;
}

int serial_read(void)
{
    if (!(inb(COM1_BASE + UART_LSR) & LSR_DATA_READY)) {
        return -1;
    }
    return (int)(inb(COM1_BASE + UART_DATA) & 0xFF);
}

void serial_init(void)
{
    outb(COM1_BASE + UART_IER, 0x00);   /* 关闭所有串口中断 */
    outb(COM1_BASE + UART_LCR, 0x80);   /* 置 DLAB，准备设置波特率 */
    outb(COM1_BASE + UART_DATA, 0x01);  /* 除数低字节 = 1 -> 115200 波特 */
    outb(COM1_BASE + UART_IER, 0x00);   /* 除数高字节 = 0 */
    outb(COM1_BASE + UART_LCR, 0x03);   /* DLAB=0, 8 位数据, 无校验, 1 停止位 */
    outb(COM1_BASE + UART_FCR, 0xC7);   /* 启用 FIFO, 清空, 14 字节阈值 */
    outb(COM1_BASE + UART_MCR, 0x0B);   /* DTR/RTS/OUT2 置位 */
}

static int serial_tx_ready(void)
{
    return inb(COM1_BASE + UART_LSR) & LSR_THR_EMPTY;
}

/* 等待发送保持寄存器空：带轮询上限，避免设备异常（THR 永久不空）时
 * 无终止自旋冻结整个系统（OSDev 红线的「无条件等待设备」反模式）。
 * 超时返回 false，调用方据此决定是否放弃本字符。 */
static bool serial_tx_wait(void)
{
    for (int i = 0; i < 100000; i++) {
        if (serial_tx_ready()) {
            return true;
        }
        cpu_relax();
    }
    return false;   /* 设备疑似卡死，放弃等待 */
}

void serial_write(char c)
{
    uint64_t f = spin_lock_irqsave(&g_serial_lock);
    /* P0-R3a：先镜像进 klog 环缓冲（含 serial_init 之前的调用也能留痕；
     * klog_dump 重放期间该函数内部抑制递归记录）。CR 转换不入环——环内
     * 保存规范化 '\n' 文本。 */
    klog_putc(c);
    if (c == '\n') {
        serial_tx_wait();
        outb(COM1_BASE + UART_DATA, '\r');   /* CRLF 换行 */
    }
    serial_tx_wait();
    outb(COM1_BASE + UART_DATA, (uint8_t)c);
    spin_unlock_irqrestore(&g_serial_lock, f);
}

void serial_writestr(const char *s)
{
    while (*s) {
        serial_write(*s++);
    }
}

void serial_write_hex(uint64_t val)
{
    static const char digits[] = "0123456789ABCDEF";
    serial_writestr("0x");
    for (int i = 60; i >= 0; i -= 4) {
        serial_write(digits[(val >> i) & 0xF]);
    }
}

void serial_write_dec(uint64_t val)
{
    char buf[21];
    int i = 20;
    buf[i--] = '\0';
    if (val == 0) {
        serial_write('0');
        return;
    }
    while (val > 0 && i >= 0) {
        buf[i--] = (char)('0' + (val % 10));
        val /= 10;
    }
    serial_writestr(&buf[i + 1]);
}
