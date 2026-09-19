/*
 * include/kernel/uhci.h
 * -----------------------------------------------------------------------------
 * UHCI (Universal Host Controller Interface, USB 1.1) 主机控制器驱动接口。
 *
 * 依据 OSDev Wiki「UHCI」条目实现：
 *   - 寄存器块（I/O 空间，BAR0 为 I/O BAR）：USBCMD/USBSTS/USBINTR/FRNUM/
 *     FRBASEADD/SOFMOD/PORTSCn。
 *   - 帧列表 1024 项（4KB 对齐）；QH(16B) / TD(32B) 结构；控制传输 =
 *     QH->SETUP TD->DATA TD*n->STATUS TD；中断传输 = QH->单 TD。
 *   - 端口复位/使能时序：SetReset -> 100ms -> ClearReset -> 50ms -> SetPED。
 *
 * 端点 0 控制传输与中断 IN 均由本驱动提供；USB 核心（usb_core.c）在其上做
 * 枚举与设备分派，HID/Hub 类驱动消费。
 */
#ifndef _SUKI_KERNEL_UHCI_H
#define _SUKI_KERNEL_UHCI_H

#include <kernel/types.h>

/* ---- UHCI 寄存器偏移（相对 I/O 基址） ---- */
#define UHCI_USBCMD     0x00   /* 2B */
#define UHCI_USBSTS     0x02   /* 2B */
#define UHCI_USBINTR    0x04   /* 2B */
#define UHCI_FRNUM      0x06   /* 2B */
#define UHCI_FRBASEADD  0x08   /* 4B */
#define UHCI_SOFMOD     0x0C   /* 1B */
#define UHCI_PORTSC1    0x10   /* 2B, PORTSCn = 0x10 + 2*(n-1) */

/* USBCMD 位 */
#define UHCI_CMD_RS         (1u << 0)   /* Run/Stop */
#define UHCI_CMD_HCRESET    (1u << 1)   /* Host Controller Reset（自清） */
#define UHCI_CMD_GRESET     (1u << 2)   /* Global Reset */
#define UHCI_CMD_CF         (1u << 6)   /* Configure Flag */
#define UHCI_CMD_MAXP       (1u << 7)   /* 1=64B max packet */

/* USBSTS 位 */
#define UHCI_STS_USBINT     (1u << 0)
#define UHCI_STS_ERROR      (1u << 1)
#define UHCI_STS_RESUME     (1u << 2)
#define UHCI_STS_HSE        (1u << 3)   /* Host System Error */
#define UHCI_STS_HCPE       (1u << 4)   /* Host Controller Process Error */
#define UHCI_STS_HCHALTED   (1u << 5)

/* PORTSC 位 */
#define UHCI_PORT_CCS       (1u << 0)   /* Current Connect Status (RO) */
#define UHCI_PORT_CSC       (1u << 1)   /* Connect Status Change (R/WC) */
#define UHCI_PORT_PED       (1u << 2)   /* Port Enabled/Disabled (R/W) */
#define UHCI_PORT_PEDC      (1u << 3)   /* Port Enable/Disable Change (R/WC) */
#define UHCI_PORT_LSDA      (1u << 8)   /* Low Speed Device Attached (RO) */
#define UHCI_PORT_RESET     (1u << 9)   /* Port Reset (R/W) */
#define UHCI_PORT_SUSPEND   (1u << 12)

/* 帧/描述符链接指针标志位 */
#define UHCI_PTR_TERM       (1u << 0)   /* 终止 */
#define UHCI_PTR_QH         (1u << 1)   /* 指向 QH */
#define UHCI_PTR_DEPTH      (1u << 2)   /* TD 深优先（继续执行本 TD 指向的下一个） */

/* PID */
#define UHCI_PID_IN         0x69
#define UHCI_PID_OUT        0xE1
#define UHCI_PID_SETUP      0x2D

/* TD 状态（CS）位 */
#define UHCI_TD_ACTLEN_MASK 0x7FFu
#define UHCI_TD_BITSTUFF    (1u << 17)
#define UHCI_TD_CRC         (1u << 18)
#define UHCI_TD_NAK         (1u << 19)
#define UHCI_TD_BABBLE      (1u << 20)
#define UHCI_TD_DBE         (1u << 21)
#define UHCI_TD_STALLED     (1u << 22)
#define UHCI_TD_ACTIVE      (1u << 23)
#define UHCI_TD_IOC         (1u << 24)
#define UHCI_TD_ISO         (1u << 25)
#define UHCI_TD_LS          (1u << 26)
#define UHCI_TD_CERR_SHIFT  27
#define UHCI_TD_CERR_3      (3u << UHCI_TD_CERR_SHIFT)
#define UHCI_TD_SPD         (1u << 29)

/* 中断传输槽位数（对应最多可同时轮询的 HID/中断 IN 端点） */
#define UHCI_MAX_INT_SLOTS  8

/* 主机控制器是否存在/已就绪 */
bool uhci_present(void);

/* 初始化 UHCI：PCI 探测、复位、帧列表、端口使能。成功返回 true。 */
bool uhci_init(void);

/* 端点 0 控制传输（阻塞、内部轮询带超时）。
 *   addr/low_speed/mps0 : 目标设备属性
 *   bmRequestType..wIndex : 标准/类请求字段
 *   data/len             : DATA 阶段缓冲（IN 时写回设备数据；OUT 时发送）
 * 返回 DATA 阶段实际字节数；失败返回负值。 */
int uhci_control(uint8_t addr, uint8_t low_speed, uint8_t mps0,
                 uint8_t bmRequestType, uint8_t bRequest,
                 uint16_t wValue, uint16_t wIndex, void *data, uint16_t len);

/* 注册一个中断 IN 端点轮询；返回槽位号（0..UHCI_MAX_INT_SLOTS-1）或 -1。
 * buf/len 为接收缓冲（内核虚拟地址，内部会拷贝）。 */
int uhci_int_add(uint8_t addr, uint8_t low_speed, uint8_t ep,
                 uint16_t mps, uint8_t interval, void *buf, uint16_t len);

/* 释放中断槽位。 */
void uhci_int_remove(int slot);

/* 轮询一个中断槽位：返回 >0 表示收到 actual 字节（已写入注册缓冲）；
 * 0 表示无数据；<0 表示端点错误/停用。内部在完成后自动重新激活。 */
int uhci_int_poll(int slot);

/* 轮询主机控制器状态：处理根端口连接变化（返回发生变化则调用 usb_core 注入）。
 * root 端口事件由本函数回调 usb_core_root_connect()。 */
void uhci_poll(void);

/* 根端口复位并使能；成功则输出低速标志。内部使用。 */
bool uhci_root_port_reset(int port, bool *low_speed);

#endif /* _SUKI_KERNEL_UHCI_H */
