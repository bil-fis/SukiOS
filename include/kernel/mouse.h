/*
 * include/kernel/mouse.h
 * -----------------------------------------------------------------------------
 * SukiOS 内核态 PS/2 鼠标采集（与键盘共用 PS/2 控制器，经 IRQ12 投递）。
 *
 * 设计依据 OSDev「PS/2 Mouse」条目标准做法：
 *   - PS/2 控制器端口：0x64=命令/状态，0x60=数据。
 *   - 向辅助设备（鼠标）发命令必须用 0xD4 前缀（写 0x64=0xD4，再写 0x60=命令）。
 *   - 标准 3 字节包：YO XO YS XS 1 M R L | dx | dy（XS/YS=符号位，YO/XO 溢出）。
 *     启用 4 字节模式（命令 0xF2 读 ID 后按 ID=3/4 扩展）时第 4 字节为滚轮。
 *   - 数据报告由 0xF4 启用；数据包由 IRQ12（GSI12）投递，内核采集进环形缓冲。
 *
 * 红线：
 *   1) 鼠标数据仅在内核态采集并放入**内核**环形缓冲；用户态经 syscall
 *      sys_mouse_read 拉取（copy_to_user 到用户缓冲），绝不允许用户态直读 0x60。
 *   2) 采集路径在 IRQ 上下文运行，不睡眠、不调用可能阻塞的接口；环形缓冲无锁
 *      （生产者单核 IRQ，消费者单 syscall 上下文，SMP 下由 irq 关中断天然串行）。
 */
#ifndef _SUKI_KERNEL_MOUSE_H
#define _SUKI_KERNEL_MOUSE_H

#include <kernel/types.h>

/*
 * mouse_packet_t —— 解析后的、对用户态友好的鼠标事件。
 * 由内核从原始 PS/2 包解析后填充，经 sys_mouse_read 拷贝给用户态。
 */
typedef struct mouse_packet {
    int32_t  dx;          /* X 方向位移（已符号扩展，负=左） */
    int32_t  dy;          /* Y 方向位移（已符号扩展，负=上；屏幕坐标下通常取反） */
    uint8_t  buttons;     /* bit0=左键, bit1=右键, bit2=中键 */
    uint8_t  wheel;       /* 滚轮增量（4 字节包模式；0=无滚轮事件） */
    uint8_t  have_wheel;  /* 当前是否启用 4 字节滚轮模式 */
} mouse_packet_t;

/* 初始化 PS/2 鼠标：探测控制器、启用辅助端口、配置采样率、启动数据报告，
 * 并注册 IRQ12 处理程序。失败返回 false（回落为无鼠标，系统不挂死）。 */
bool mouse_init(void);

/* 非阻塞取一个已解析的鼠标包；无数据返回 false（调用方应 sys_yield 后重试）。
 * 供 syscall 层 sys_mouse_read 调用。 */
bool mouse_get_packet(mouse_packet_t *out);

#endif /* _SUKI_KERNEL_MOUSE_H */
