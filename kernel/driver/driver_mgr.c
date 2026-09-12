/*
 * kernel/driver/driver_mgr.c
 * -----------------------------------------------------------------------------
 * 驱动管理器（Driver Manager）
 *
 * 职责：
 *   - 维护已注册内核驱动（driver_t）的全局链表；
 *   - driver_register() 时触发「双向匹配」：让本驱动尝试绑定当前所有未绑定设备
 *     （device_try_bind_driver，由 device_mgr 实现）；
 *   - 一套驱动（单例 driver_t）可经匹配对【多个】设备各 probe 一次，driver->nbound
 *     累加——这是「一套驱动驱动多个设备」的核心。
 *
 * 注意：driver 链表锁（g_dlock）与 device 链表锁不在同一层级，且本文件只在持有
 * driver 锁后「单向」调用 device 侧入口，device 侧不会反向申请 driver 锁，故无死锁。
 */
#include <kernel/types.h>
#include <kernel/console.h>
#include <kernel/spinlock.h>
#include <mm/kmalloc.h>
#include <sukios/kdr.h>

static driver_t *g_drivers = NULL;
static spinlock_t g_dlock;

driver_t *driver_list_head(void)
{
    return g_drivers;
}

void driver_manager_init(void)
{
    g_drivers = NULL;
    spinlock_init(&g_dlock, "driver-mgr");
    kprintf("[driver] manager initialized\n");
}

int driver_register(driver_t *drv)
{
    if (!drv)
        return -1;
    drv->nbound = 0;
    spin_lock(&g_dlock);
    drv->next = g_drivers;
    g_drivers = drv;
    spin_unlock(&g_dlock);

    kprintf("[driver] registered '%s'\n", drv->name ? drv->name : "(null)");

    /* 双向匹配：本驱动尝试绑定所有已注册但未绑定的设备 */
    device_try_bind_driver(drv);
    return 0;
}

void driver_unregister(driver_t *drv)
{
    if (!drv)
        return;
    spin_lock(&g_dlock);
    driver_t **pp = &g_drivers;
    while (*pp) {
        if (*pp == drv) {
            *pp = drv->next;
            break;
        }
        pp = &(*pp)->next;
    }
    spin_unlock(&g_dlock);
    kprintf("[driver] unregistered '%s' (was bound to %d devices)\n",
            drv->name ? drv->name : "(null)", drv->nbound);
}

/* 导出 driver_register，供 .kdr 内核模块在 kdr_init 中注册驱动 */
EXPORT_SYMBOL(driver_register);
