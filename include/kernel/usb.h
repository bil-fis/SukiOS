/*
 * include/kernel/usb.h
 * -----------------------------------------------------------------------------
 * USB 核心：描述符结构、标准请求、设备表、枚举与即插即用分派。
 *
 * 依据 OSDev Wiki「USB」「USB Hubs」「USB Human Input Devices」条目：
 *   - 默认控制管道：GET_DESCRIPTOR / SET_ADDRESS / SET_CONFIGURATION 等标准请求。
 *   - 设备枚举：读 8B 设备描述符 -> SET_ADDRESS -> 读全描述符 -> 读配置 -> SET_CONFIG。
 *   - 类分派：HID（键盘/鼠标，boot 协议）、Hub（下行端口管理）。
 */
#ifndef _SUKI_KERNEL_USB_H
#define _SUKI_KERNEL_USB_H

#include <kernel/types.h>

/* 类代码 */
#define USB_CLASS_HID           0x03
#define USB_CLASS_HUB           0x09
#define USB_CLASS_MASS_STORAGE  0x08

/* 描述符类型 */
#define USB_DT_DEVICE     0x01
#define USB_DT_CONFIG     0x02
#define USB_DT_STRING     0x03
#define USB_DT_INTERFACE  0x04
#define USB_DT_ENDPOINT   0x05
#define USB_DT_HID        0x21
#define USB_DT_REPORT     0x22
#define USB_DT_HUB        0x29

/* 标准请求 */
#define USB_REQ_GET_STATUS         0x00
#define USB_REQ_CLEAR_FEATURE      0x01
#define USB_REQ_SET_FEATURE        0x03
#define USB_REQ_SET_ADDRESS        0x05
#define USB_REQ_GET_DESCRIPTOR     0x06
#define USB_REQ_SET_DESCRIPTOR     0x07
#define USB_REQ_GET_CONFIGURATION  0x08
#define USB_REQ_SET_CONFIGURATION  0x09

/* bmRequestType 位 */
#define USB_DIR_OUT        0x00
#define USB_DIR_IN         0x80
#define USB_TYPE_STANDARD  0x00
#define USB_TYPE_CLASS     0x20
#define USB_RECIP_DEVICE   0x00
#define USB_RECIP_INTERFACE 0x01
#define USB_RECIP_ENDPOINT 0x02
#define USB_RECIP_OTHER    0x03

/* HID 类请求 */
#define USB_HID_SET_IDLE     0x0A
#define USB_HID_SET_PROTOCOL 0x0B

/* Hub 类请求 */
#define USB_HUB_GET_STATUS        0x00
#define USB_HUB_CLEAR_FEATURE     0x01
#define USB_HUB_SET_FEATURE       0x03
#define USB_HUB_GET_DESCRIPTOR    0x06
/* Hub 端口 feature 选择子 */
#define USB_HUB_FEAT_PORT_CONNECTION  0
#define USB_HUB_FEAT_PORT_ENABLE      1
#define USB_HUB_FEAT_PORT_RESET       4
#define USB_HUB_FEAT_PORT_POWER       8
#define USB_HUB_FEAT_C_PORT_CONNECTION 16
#define USB_HUB_FEAT_C_PORT_ENABLE     17
#define USB_HUB_FEAT_C_PORT_RESET      20

/* 设备状态 */
#define USB_STATE_FREE       0
#define USB_STATE_ENUMERATED 1

/* 类驱动种类 */
#define USB_KIND_NONE   0
#define USB_KIND_HID_KBD 1
#define USB_KIND_HID_MOUSE 2
#define USB_KIND_HUB    3

typedef struct __attribute__((packed)) usb_device_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} usb_device_desc_t;

typedef struct __attribute__((packed)) usb_config_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} usb_config_desc_t;

typedef struct __attribute__((packed)) usb_interface_desc {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} usb_interface_desc_t;

typedef struct __attribute__((packed)) usb_endpoint_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} usb_endpoint_desc_t;

/* 每个 USB 设备在核心表中的条目 */
#define USB_MAX_DEVICES 16

typedef struct usb_device {
    uint8_t  state;          /* USB_STATE_* */
    uint8_t  address;        /* 1..127 */
    uint8_t  low_speed;
    uint8_t  max_packet0;
    uint8_t  dev_class;
    uint8_t  config_value;
    uint8_t  kind;           /* USB_KIND_* */
    /* 所属上行端口：parent_hub<0 表示挂在主机控制器根端口 */
    int      parent_hub;     /* usb_device 下标，或 -1 */
    uint8_t  parent_port;    /* 1-based 端口号 */
    /* HID / 中断端点信息 */
    uint8_t  int_slot;       /* uhci_int_add 返回的槽位，0xFF 未用 */
    uint8_t  int_ep;         /* 中断 IN 端点号（含方向位） */
    uint16_t int_mps;        /* 中断端点最大包长 */
    uint8_t  hid_protocol;   /* 1=键盘 2=鼠标 */
    /* Hub 信息 */
    uint8_t  hub_ports;
    uint8_t  hub_port_prev[16]; /* 每端口上次连接状态（>16 截断） */
} usb_device_t;

/* USB 核心入口/轮询（由 kmain / sched_tick 调用） */
bool usb_init(void);
void usb_poll(void);

/* 供主机控制器驱动回调：根端口出现新设备（已完成复位使能）。
 * port 为 1-based 根端口号。返回新设备下标或 -1。 */
int usb_core_root_connect(int port, bool low_speed);
/* 根端口设备移除。 */
void usb_core_root_disconnect(int port);

/* 供 Hub 类驱动回调：下行端口出现新设备（父=hub_idx）。
 * 返回新设备下标或 -1。 */
int usb_core_hub_connect(int hub_idx, int port, bool low_speed);
void usb_core_hub_disconnect(int hub_idx, int port);

/* 取得设备表项（供类驱动读取低层属性）。 */
usb_device_t *usb_core_get(int idx);

#endif /* _SUKI_KERNEL_USB_H */
