/* =============================================================================
 * SukiOS - UART 串口驱动
 *
 * 使用 COM1 (0x3F8) 输出日志，用于调试。
 * 参考：OSDev Serial Ports, 8250/16550 UART datasheet
 * ============================================================================= */

#ifndef SUKIOS_UART_H
#define SUKIOS_UART_H

/* COM1 端口基址 */
#define COM1_BASE   0x3F8

/* UART 寄存器偏移 */
#define UART_RBR    0   /* 接收缓冲寄存器（读） */
#define UART_THR    0   /* 发送保持寄存器（写） */
#define UART_IER    1   /* 中断使能寄存器 */
#define UART_IIR    2   /* 中断识别寄存器（读） */
#define UART_FCR    2   /* FIFO 控制寄存器（写） */
#define UART_LCR    3   /* 线路控制寄存器 */
#define UART_MCR    4   /* 调制解调控制寄存器 */
#define UART_LSR    5   /* 线路状态寄存器 */
#define UART_DLL    0   /* 除数锁存器低字节（LCR.DLAB=1） */
#define UART_DLM    1   /* 除数锁存器高字节（LCR.DLAB=1） */

/* LSR 位 */
#define UART_LSR_TX_READY   (1 << 5)  /* 发送保持寄存器为空 */

/* LCR 位 */
#define UART_LCR_DLAB       (1 << 7)  /* 除数锁存器访问位 */
#define UART_LCR_8BIT       0x03      /* 8 位数据，无校验，1 停止位 */

/* MCR 位 */
#define UART_MCR_DTR        (1 << 0)
#define UART_MCR_RTS        (1 << 1)
#define UART_MCR_OUT2       (1 << 3)  /* 允许中断输出（有些 UART 需要） */

/* FCR 位 */
#define UART_FCR_ENABLE     (1 << 0)  /* 启用 FIFO */
#define UART_FCR_CLEAR_RX   (1 << 1)  /* 清空接收 FIFO */
#define UART_FCR_CLEAR_TX   (1 << 2)  /* 清空发送 FIFO */

/**
 * uart_init - 初始化 COM1 串口
 *
 * 配置：115200 波特率，8N1（8 数据位，无校验，1 停止位）
 * 步骤（参考 OSDev Serial Ports）：
 *   1. 禁用中断
 *   2. 设置 DLAB=1，配置波特率除数（115200 = 115200 / 1 → 除数=1）
 *   3. 设置 8N1 数据格式
 *   4. 启用 FIFO，清空缓冲
 *   5. 设置 MCR（DTR + RTS + OUT2）
 */
void uart_init(void);

/**
 * uart_putchar - 通过串口发送一个字符
 * @c: 要发送的字符
 *
 * 阻塞等待发送缓冲区就绪后发送。
 */
void uart_putchar(char c);

#endif /* SUKIOS_UART_H */
