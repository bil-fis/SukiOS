/*
 * kernel/syscall/sys_suki.c — SukiNative 原生对象 API 分发（130..199）
 *
 * Phase 0 交付范围：仅完成号位预留与分发路由。本文件是 SukiNative 系统调用的
 * 统一入口；当前所有号均返回 -ENOSYS（未实现）。Phase 1 起在此实现具体的对象/
 * 句柄子系统（suki_object_t、每进程句柄表、SYS_SUKI_WAIT 多对象等待、事件/互斥/
 * 信号量等），届时按 num 分流到各 handler。
 *
 * 设计要点（对照 SukiNative API 规范）：
 *   - 一切皆对象，用户态拿到 suki_handle_t 句柄（类 Mach 能力/端口），不直接
 *     操作内核指针；
 *   - 返回 suki_status_t 状态码（success/err 两位编码），而非 POSIX 的 errno；
 *   - POSIX 兼容层（20..129）对外行为不变，新服务可直接使用本原生接口。
 *
 * 注意：从用户态接收的指针（如路径、缓冲区）必须在 handler 内用
 * copy_from_user/copy_to_user 访问，禁止直接解引用（见项目强制规则）。
 */

#include <kernel/syscall.h>
#include <sukios/posix.h>

uint64_t sys_suki_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)num;
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    /* Phase 0：号位已预留（见 posix.h 的 SYS_SUKI_*），具体对象子系统在 Phase 1
     * 实现。未实现返回 -ENOSYS（内核无 ENOSYS 宏，等价用 SUKI_ENOSYS=38）；
     * POSIX（20..129）不受影响，继续由各 POSIX handler 处理。 */
    return (uint64_t)-SUKI_ENOSYS;
}
