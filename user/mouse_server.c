/*
 * user/mouse_server.c
 * -----------------------------------------------------------------------------
 * SukiOS Ring3 鼠标驱动（.kdr 形态，待 kdr 加载器就绪后改为独立可加载模块）。
 *
 * 职责（符合混合内核模型：驱动即 Ring3 服务，经 mach_msg 通信）：
 *   1) 经内核 syscall sys_mouse_read 拉取 IRQ12 采集的鼠标事件
 *      （dx/dy/buttons/wheel，由内核解析，用户态绝不直读 0x60）；
 *   2) 维护绝对光标坐标（累计位移，clamp 到合理范围）；
 *   3) 把光标事件经 MOUSE_PORT 发给 display_server，由其绘制/移动光标。
 *
 * 启动由 kmain 经 task_create_user 内嵌拉起（与 input-server 同形态）；
 * 待 kdr 加载器完成后，本文件编译为 build/user/<name>.kdr.blob.o 并由加载器
 * 经 sys_task_spawn 动态装载，无需内核改动。
 *
 * 红线：
 *   - 仅用合法 syscall 与 mach_msg，绝不 inb/outb（Ring3 无 IO 权限，会 #GP）。
 *   - 用户指针（sys_mouse_read 的 pkt）由内核 copy_to_user 填，本进程在内核态
 *     之外的地址空间内访问，安全。
 */
#include <stdint.h>
#include <stdbool.h>
#include <sukios/posix.h>
#include "lib/suki.h"

/*
 * 鼠标事件包：必须与内核 include/kernel/mouse.h 的 mouse_packet_t **逐字节布局一致**
 * （跨特权边界由内核 copy_to_user 填，错位会导致 dx/dy/buttons 读错、用户态解析异常）。
 * 内核布局（含自然对齐填充）：
 *   offset 0  : int32_t dx
 *   offset 4  : int32_t dy
 *   offset 8  : uint8_t buttons
 *   offset 9  : uint8_t wheel
 *   offset 10 : uint8_t have_wheel
 *   offset 11 : (pad 1) -> 共 12 字节
 */
typedef struct mouse_packet {
    int32_t  dx;
    int32_t  dy;
    uint8_t  buttons;
    uint8_t  wheel;
    uint8_t  have_wheel;
    uint8_t  _pad;
} mouse_packet_t;


/* 鼠标事件消息：mouse_server -> display_server（经 DISPLAY_PORT 投递）。
 * msgh_id 区分事件类型，且必须与 display_server 的 id 分区一致：
 *   文本 DISP_MSG_TEXT = 1（由内核 user_puts 转发），若鼠标也用 1 会造成歧义，
 *   故鼠标事件采用独立 id 段（>=100），与文本彻底隔离。
 * 坐标由 mouse_server 维护（绝对坐标，默认 1024x768 基准），display_server
 * 收到后用自身实际分辨率二次 clamp 后绘制光标。 */
#define MOUSE_MSG_MOVE  101   /* 光标移动（含按钮状态） */
#define MOUSE_MSG_BUTTON 102  /* 按钮状态变化（按下/松开） */
#define MOUSE_MSG_WHEEL 103   /* 滚轮事件 */

typedef struct mouse_event_msg {
    mach_msg_header_t h;
    int32_t  x;          /* 当前绝对光标 X */
    int32_t  y;          /* 当前绝对光标 Y */
    uint8_t  buttons;    /* 当前按钮位图：bit0=左,1=右,2=中 */
    int8_t   wheel;      /* 滚轮增量（有符号） */
    uint8_t  _pad[3];
} mouse_event_msg_t;

#define SCREEN_W_DEFAULT 1024
#define SCREEN_H_DEFAULT 768
#define CURSOR_X_INIT  (SCREEN_W_DEFAULT / 2)
#define CURSOR_Y_INIT  (SCREEN_H_DEFAULT / 2)

static int32_t g_cx = CURSOR_X_INIT;
static int32_t g_cy = CURSOR_Y_INIT;
static uint8_t g_buttons = 0;

/* 把绝对坐标 clamp 到 [0, max)，防御鼠标大幅跳变/累加溢出 */
static int32_t clamp_i32(int32_t v, int32_t max)
{
    if (v < 0)   return 0;
    if (v >= max) return max - 1;
    return v;
}

static void send_event(uint32_t id)
{
    mouse_event_msg_t m;
    u_memset(&m, 0, sizeof(m));
    m.h.msgh_bits        = MACH_SEND_MSG;
    m.h.msgh_size        = sizeof(m);
    m.h.msgh_remote_port = DISPLAY_PORT;
    m.h.msgh_local_port  = 0;
    m.h.msgh_id          = id;
    m.x = g_cx;
    m.y = g_cy;
    m.buttons = g_buttons;
    /* wheel 在 MOUSE_MSG_WHEEL 时由调用方预设 */
    mach_msg_send(&m, sizeof(m));
}

int main(void)
{
    u_print("[mouse-srv] Ring3 mouse driver online (kdr-form), consuming "
            "SYS_MOUSE_READ\n");

    /* 认领 MOUSE_PORT 的接收权（便于将来双向，当前主要作发送端口）。
     * 若端口已被占用（未来 kdr 加载器多实例场景）则忽略错误继续发送。 */
    sys_port_claim(MOUSE_PORT);

    mouse_packet_t pkt;
    uint64_t idle = 0;
    bool first_event_logged = false;
    for (;;) {
        long rc = sys_mouse_read(&pkt);
        if (rc != 0) {
            /* 无事件：让出 CPU，避免空转烧核；短暂退避 */
            idle++;
            if (idle > 64) {
                idle = 0;
            }
            sys_yield();
            continue;
        }
        idle = 0;

        /* 首事件诊断（仅打印一次）：验证 IRQ12 -> syscall -> Ring3 全链路贯通。
         * 后续事件不再打印，避免刷屏。 */
        if (!first_event_logged) {
            first_event_logged = true;
            char b1[24], b2[24], b3[24];
            int32_t sdx = pkt.dx, sdy = pkt.dy;
            u_utoa_s((uint64_t)(sdx < 0 ? -sdx : sdx), b1, sizeof(b1));
            u_utoa_s((uint64_t)(sdy < 0 ? -sdy : sdy), b2, sizeof(b2));
            u_utoa_s((uint64_t)pkt.buttons, b3, sizeof(b3));
            u_print("[mouse-srv] first event: dx=");
            u_print(sdx < 0 ? "-" : "");
            u_print(b1);
            u_print(" dy=");
            u_print(sdy < 0 ? "-" : "");
            u_print(b2);
            u_print(" buttons=");
            u_print(b3);
            u_print("\n");
        }

        /* 累计位移 -> 绝对坐标（屏幕 Y 轴通常向下为正，dy 已按 PS/2 语义，
         * OSDev 规定 dy 向下为正；此处直接累加作为"屏幕坐标向下"） */
        g_cx = clamp_i32(g_cx + pkt.dx, SCREEN_W_DEFAULT);
        g_cy = clamp_i32(g_cy + pkt.dy, SCREEN_H_DEFAULT);

        uint8_t prev = g_buttons;
        g_buttons = pkt.buttons & 0x07;

        /* 按钮变化 -> 单独事件 */
        if (g_buttons != prev) {
            send_event(MOUSE_MSG_BUTTON);
        }
        /* 滚轮事件 */
        if (pkt.have_wheel && pkt.wheel != 0) {
            mouse_event_msg_t m;
            u_memset(&m, 0, sizeof(m));
            m.h.msgh_bits        = MACH_SEND_MSG;
            m.h.msgh_size        = sizeof(m);
            m.h.msgh_remote_port = DISPLAY_PORT;
            m.h.msgh_local_port  = 0;
            m.h.msgh_id          = MOUSE_MSG_WHEEL;
            m.x = g_cx; m.y = g_cy;
            m.buttons = g_buttons;
            m.wheel = (int8_t)pkt.wheel;
            mach_msg_send(&m, sizeof(m));
        }
        /* 移动事件（含最新坐标与按钮） */
        send_event(MOUSE_MSG_MOVE);
    }
    return 0;
}
