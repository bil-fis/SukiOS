/*
 * include/kernel/usb_hid.h
 * -----------------------------------------------------------------------------
 * USB HID boot 协议类驱动（键盘/鼠标）。由 usb_core 在枚举到 HID 接口后调用。
 * 依据 OSDev Wiki「USB Human Input Devices」：boot 协议下键盘报告 8 字节
 * （修饰键 + 保留 + 6 键码），鼠标报告 3 字节（按键 + dx + dy）。
 */
#ifndef _SUKI_KERNEL_USB_HID_H
#define _SUKI_KERNEL_USB_HID_H

#include <kernel/types.h>
#include <kernel/usb.h>

/* 启动 HID 设备：发 SET_IDLE/SET_PROTOCOL(boot)，注册中断 IN 端点并把
 * 槽位写入 dev->int_slot。前置：usb_core 已填好 address/low_speed/
 * max_packet0/int_ep/int_mps/hid_protocol。成功返回 true。 */
bool usb_hid_start(int dev_idx, uint8_t interface_num);

/* 轮询该 HID 设备的中断端点，解析报告并注入 PS/2 键盘/鼠标事件。
 * 返回 >0=本次收到字节数，0=无数据，-1=端点致命错误（调用方应释放设备）。 */
int usb_hid_poll(int dev_idx);

#endif /* _SUKI_KERNEL_USB_HID_H */
