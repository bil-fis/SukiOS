/* =============================================================================
 * SukiOS - UART 串口驱动实现
 *
 * 使用 COM1 (I/O 端口 0x3F8) 进行日志输出。
 * 参考：OSDev Serial Ports, Intel 8250/16550 UART Programmer's Manual
 * ============================================================================= */

#include "kernel/uart.h"
#include "kernel/io.h"

void uart_init(void)
{
    /* ---- 1. 禁用所有中断 ---- */
    outb(COM1_BASE + UART_IER, 0x00);

    /* ---- 2. 设置波特率 115200 ----
     *
     * UART 时钟基准频率 = 1.8432 MHz
     * 波特率 = 基准频率 / (16 × 除数)
     * 115200 = 1843200 / (16 × 1) → 除数 = 1
     *
     * 先设置 DLAB=1 以访问除数锁存器 */
    outb(COM1_BASE + UART_LCR, UART_LCR_DLAB);
    outb(COM1_BASE + UART_DLL, 0x01);  /* 除数低字节 = 1 */
    outb(COM1_BASE + UART_DLM, 0x00);  /* 除数高字节 = 0 */

    /* ---- 3. 配置 8N1 数据格式 ----
     * 8 位数据位，无校验，1 停止位，DLAB=0 */
    outb(COM1_BASE + UART_LCR, UART_LCR_8BIT);

    /* ---- 4. 启用 FIFO，清空收发缓冲 ---- */
    outb(COM1_BASE + UART_FCR,
         UART_FCR_ENABLE | UART_FCR_CLEAR_RX | UART_FCR_CLEAR_TX);

    /* ---- 5. 设置 MCR ----
     * DTR=1, RTS=1, OUT2=1（OUT2 在某些 16550 上必须置位才能正确工作） */
    outb(COM1_BASE + UART_MCR, UART_MCR_DTR | UART_MCR_RTS | UART_MCR_OUT2);
}

void uart_putchar(char c)
{
    /* 等待发送保持寄存器就绪（LSR bit 5 = THRE） */
    while (!(inb(COM1_BASE + UART_LSR) & UART_LSR_TX_READY))
        ;

    outb(COM1_BASE + UART_THR, (uint8_t)c);
}
