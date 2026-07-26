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

#define COM1_BASE   0x3F8

/* 16550 寄存器偏移 */
#define UART_DATA        0   /* DLAB=0: 数据寄存器 / DLAB=1: 除数低字节 */
#define UART_IER         1   /* DLAB=0: 中断使能 / DLAB=1: 除数高字节 */
#define UART_FCR         2   /* FIFO 控制 */
#define UART_LCR         3   /* 线路控制 */
#define UART_MCR         4   /* Modem 控制 */
#define UART_LSR         5   /* 线路状态 */

#define LSR_THR_EMPTY    0x20  /* 发送保持寄存器空 */

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

void serial_write(char c)
{
    if (c == '\n') {
        while (!serial_tx_ready()) { }
        outb(COM1_BASE + UART_DATA, '\r');   /* CRLF 换行 */
    }
    while (!serial_tx_ready()) { }
    outb(COM1_BASE + UART_DATA, (uint8_t)c);
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
