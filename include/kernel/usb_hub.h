/*
 * include/kernel/usb_hub.h
 * -----------------------------------------------------------------------------
 * USB Hub 类驱动：读 Hub 描述符、给下行端口供电、轮询端口状态变化并触发
 * 下游设备的枚举/移除（即插即用）。依据 OSDev Wiki「USB Hubs」。
 */
#ifndef _SUKI_KERNEL_USB_HUB_H
#define _SUKI_KERNEL_USB_HUB_H

#include <kernel/types.h>
#include <kernel/usb.h>

/* 初始化 Hub：读 Hub 描述符得端口数，为所有端口供电。成功返回 true。 */
bool usb_hub_start(int dev_idx);

/* 轮询 Hub 下行端口：处理连接/断开变化（复位端口 -> 枚举下游设备）。 */
void usb_hub_poll(int dev_idx);

/* 下游设备移除时清理（由 usb_core 调用）。 */
void usb_hub_cleanup(int dev_idx);

#endif /* _SUKI_KERNEL_USB_HUB_H */
