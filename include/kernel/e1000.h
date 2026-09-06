/*
 * include/kernel/e1000.h
 * -----------------------------------------------------------------------------
 * Intel 8254x (e1000) 千兆以太网控制器 —— 寄存器、描述符与驱动接口。
 *
 * 依据：本地 osdev_wiki 离线副本 `wiki.osdev.org/Intel_8254x`（寄存器表、
 *       Initialization / Ring setup / Interrupt Handling / Packet
 *       Transmittion / Packet Reception 各节），以及 Intel 8254x Software
 *       Developer's Manual（Table 13-2 寄存器表）。
 *
 * 设计要点：
 *   - 仅使用 BAR0 的 MMIO 访问（osdev：BAR0 在本系列各设备中编号恒定）；
 *   - EEPROM 经 EERD 寄存器读取前 3 个 word 得到 MAC（osdev Initialization 节）；
 *   - 描述符用 legacy 16 字节格式，TX 8 项 / RX 32 项，每项缓冲 4096 字节；
 *   - 收发采用轮询（与 hda.c 一致的稳健策略，避免中断路由差异带来的风险），
 *     通过描述符的 DD（Descriptor Done）位判定完成；
 *   - 驱动只做原始帧收发，协议栈（lwIP）在用户态，符合混合内核红线。
 *
 * 调用关系：kmain -> e1000_init() + net_srv_start()；
 *           网络服务 --mach_msg--> NET_PORT -> e1000_send/e1000_recv。
 */
#ifndef _SUKI_KERNEL_E1000_H
#define _SUKI_KERNEL_E1000_H

#include <kernel/types.h>

/* ---- 设备标识 ---- */
#define E1000_VENDOR_INTEL      0x8086

/* ---- 寄存器偏移（osdev "Device Registers" 表） ---- */
#define E1000_REG_CTRL      0x00000u   /* Device Control            R/W */
#define E1000_REG_STATUS    0x00008u   /* Device Status             R   */
#define E1000_REG_EECD      0x00010u   /* EEPROM/Flash Control/Data R/W */
#define E1000_REG_EERD      0x00014u   /* EEPROM Read               R/W */
#define E1000_REG_ICR       0x000C0u   /* Interrupt Cause Read      R/W */
#define E1000_REG_IMS       0x000D0u   /* Interrupt Mask Set/Read   R/W */
#define E1000_REG_IMC       0x000D8u   /* Interrupt Mask Clear      W   */
#define E1000_REG_RCTL      0x00100u   /* Receive Control           R/W */
#define E1000_REG_TCTL      0x00400u   /* Transmit Control          R/W */
#define E1000_REG_TIPG      0x00410u   /* Transmit IPG              R/W */
#define E1000_REG_RDBAL     0x02800u   /* Rx Descriptor Base Low    R/W */
#define E1000_REG_RDBAH     0x02804u   /* Rx Descriptor Base High   R/W */
#define E1000_REG_RDLEN     0x02808u   /* Rx Descriptor Length      R/W */
#define E1000_REG_RDH       0x02810u   /* Rx Descriptor Head        R/W */
#define E1000_REG_RDT       0x02818u   /* Rx Descriptor Tail        R/W */
#define E1000_REG_TDBAL     0x03800u   /* Tx Descriptor Base Low    R/W */
#define E1000_REG_TDBAH     0x03804u   /* Tx Descriptor Base High   R/W */
#define E1000_REG_TDLEN     0x03808u   /* Tx Descriptor Length      R/W */
#define E1000_REG_TDH       0x03810u   /* Tx Descriptor Head        R/W */
#define E1000_REG_TDT       0x03818u   /* Tx Descriptor Tail        R/W */
#define E1000_REG_MTA       0x05200u   /* Multicast Table Array     R/W */
#define E1000_REG_RAL0      0x05400u   /* Receive Address Low 0     R/W */
#define E1000_REG_RAH0      0x05404u   /* Receive Address High 0    R/W */

/* ---- CTRL（Device Control）位 ---- */
#define E1000_CTRL_RST      (1u << 26)  /* 设备复位（自清） */
#define E1000_CTRL_ASDE     (1u << 5)   /* 自动速率检测使能 */
#define E1000_CTRL_SLU      (1u << 6)   /* Set Link Up */

/* ---- STATUS 位 ---- */
#define E1000_STATUS_LU     (1u << 1)   /* Link Up */

/* ---- EERD（EEPROM Read）位 ---- */
#define E1000_EERD_START    (1u << 0)   /* 启动读 */
#define E1000_EERD_DONE     (1u << 4)   /* 读完成（82540EM/82545EM） */
#define E1000_EERD_ADDR_SHIFT 8         /* 地址位于 bit 15:8 */
#define E1000_EERD_DATA_SHIFT 16        /* 数据位于 bit 31:16 */

/* ---- RCTL（Receive Control）位 ---- */
#define E1000_RCTL_EN       (1u << 1)   /* 接收使能 */
#define E1000_RCTL_LPE      (1u << 5)   /* 长包接收使能 */
#define E1000_RCTL_BAM      (1u << 15)  /* 广播接收使能 */
#define E1000_RCTL_BSIZE_SHIFT 16       /* 缓冲大小字段 bit 17:16 */
#define E1000_RCTL_BSEX     (1u << 25)  /* 缓冲大小扩展 */
#define E1000_RCTL_SECRC    (1u << 26)  /* 剥离 CRC（便于协议栈直接取用） */

/* ---- TCTL（Transmit Control）位 ---- */
#define E1000_TCTL_EN       (1u << 1)   /* 发送使能 */
#define E1000_TCTL_PSP      (1u << 3)   /* 填充短包 */
#define E1000_TCTL_CT_SHIFT 4           /* 碰撞阈值 bit 11:4 */
#define E1000_TCTL_COLD_SHIFT 12        /* 全双工碰撞距离 bit 21:12 */

/* ---- 发送描述符命令位 ---- */
#define E1000_TX_CMD_EOP    (1u << 0)   /* 包结束 */
#define E1000_TX_CMD_IFCS   (1u << 1)   /* 插入 FCS */
#define E1000_TX_CMD_RS     (1u << 3)   /* 报告状态 */

/* ---- 接收描述符状态位 ---- */
#define E1000_RX_STATUS_DD  (1u << 0)   /* Descriptor Done */
#define E1000_RX_STATUS_EOP (1u << 1)   /* 包结束 */

/* ---- 发送描述符状态位 ---- */
#define E1000_TX_STATUS_DD  (1u << 0)   /* Descriptor Done */

/* ---- 环与缓冲尺寸 ---- */
#define E1000_TX_DESC_COUNT 8u
#define E1000_RX_DESC_COUNT 32u
#define E1000_BUFFER_SIZE   4096u

/* ---- legacy 发送描述符（16 字节） ---- */
typedef struct e1000_tx_desc {
    uint64_t addr;      /* 数据缓冲物理地址 */
    uint16_t length;    /* 本描述符数据长度 */
    uint8_t  cso;       /* 校验和偏移 */
    uint8_t  cmd;       /* 命令（EOP/IFCS/RS） */
    uint8_t  status;    /* 状态（DD） */
    uint8_t  css;       /* 校验和起始 */
    uint16_t special;
} __attribute__((packed)) e1000_tx_desc_t;

/* ---- legacy 接收描述符（16 字节） ---- */
typedef struct e1000_rx_desc {
    uint64_t addr;      /* 数据缓冲物理地址 */
    uint16_t length;    /* 收到的字节数 */
    uint16_t checksum;  /* 硬件校验和 */
    uint8_t  status;    /* 状态（DD/EOP） */
    uint8_t  errors;    /* 错误 */
    uint16_t special;
} __attribute__((packed)) e1000_rx_desc_t;

/* ---- 驱动接口 ---- */

/* 探测并初始化 e1000（PCI 枚举 -> 复位 -> 读 MAC -> 建环 -> 使能收发）。
 * 成功返回 true（g_present 置位），未找到网卡或初始化失败返回 false。 */
bool e1000_init(void);

/* 是否已探测到可用的 e1000。 */
bool e1000_present(void);

/* 取 MAC 地址（6 字节）。未初始化返回 false。 */
bool e1000_get_mac(uint8_t mac[6]);

/* 链路是否已连接（STATUS.LU）。 */
bool e1000_link_up(void);

/* 发送一帧。length 必须 1..NET_MAX_FRAME。成功返回 true。 */
bool e1000_send(const void *data, uint32_t length);

/* 接收一帧到 out（最多 max 字节）。
 * 返回：>0 实际帧长度；0 表示当前无帧；-1 表示驱动不可用。 */
int  e1000_recv(uint8_t *out, uint32_t max);

/* 启动 NET_PORT 内核服务任务（响应原始帧收发请求）。 */
void net_srv_start(void);

#endif /* _SUKI_KERNEL_E1000_H */
