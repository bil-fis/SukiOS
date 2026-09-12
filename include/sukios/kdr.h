/*
 * include/sukios/kdr.h
 * -----------------------------------------------------------------------------
 * SukiOS 内核模块（.kdr，Ring0 内核驱动）的编程接口——内核侧与模块侧共用同一
 * 份定义，保证结构体布局 / 函数原型在两侧完全一致（否则跨模块调用会 ABI 错乱）。
 *
 * 角色：
 *   - 内核：实现 driver_manager / device_manager，并 EXPORT_SYMBOL 若干 API；
 *   - .kdr 模块（用交叉 gcc 编成 ET_DYN 共享对象）：include 本头，实现
 *     kdr_init()/kdr_exit()，在其中构造 driver_t 并调用 driver_register()。
 *
 * 「一套驱动驱动多个设备」：driver_t 是单例（一份 match/probe 函数），却可
 * 通过 device_manager 的双向匹配，对【多个】device_t 各调用一次 probe()，
 * 每次 probe 用 kmalloc 分配独立 drvdata 挂到 dev->drvdata，driver->nbound 随之
 * 累加——这正是本头定义的核心抽象。
 */
#ifndef _SUKI_SUKIOS_KDR_H
#define _SUKI_SUKIOS_KDR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* 设备所在总线类型 */
typedef enum {
    BUS_VIRTUAL = 0,   /* 内核/模块注册的虚拟/演示设备 */
    BUS_PCI     = 1,   /* PCI 枚举得到的真实硬件设备 */
} bus_type_t;

struct driver;

/* 设备实例（每个物理/虚拟设备一个，由 device_manager 统一管理） */
typedef struct device {
    uint32_t       dev_id;       /* 设备实例唯一 id（内核分配） */
    bus_type_t     bus;          /* 总线类型 */
    uint16_t       vendor;       /* PCI vendor（虚拟设备填 0） */
    uint16_t       device;       /* PCI device id */
    uint8_t        class_code;   /* PCI class（虚拟设备用 0x80+ 自定义类） */
    uint8_t        subclass;     /* PCI subclass */
    uint8_t        bus_no;       /* PCI 拓扑 */
    uint8_t        dev_no;
    uint8_t        func;
    uint64_t       mmio_base;    /* 资源（MMIO 物理基址，0 表示无） */
    const char    *name;         /* 设备名（诊断用） */
    struct driver *driver;       /* 已绑定驱动（NULL = 未绑定） */
    void          *drvdata;      /* 驱动私有数据（probe 时分配） */
    uint32_t       ref;          /* 保留 */
    struct device *next;         /* 设备链表（device_manager 内部使用） */
} device_t;

/* 驱动（单例；一份 match/probe 可服务多个设备） */
typedef struct driver {
    const char *name;
    /* 返回 true 表示本驱动可驱动该设备 */
    bool (*match)(const device_t *dev);
    /* 匹配成功后调用，建立实例；返回 0 表示成功绑定 */
    int  (*probe)(device_t *dev);
    /* 设备移除时调用（可选） */
    void (*remove)(device_t *dev);
    void  *priv;
    int    nbound;     /* 已绑定设备数（一套驱动驱动多个设备的直接体现） */
    struct driver *next;
} driver_t;

/* ---- 内核导出给 .kdr 模块的内核 API（经 EXPORT_SYMBOL 可见）---- */
extern void  kprintf(const char *fmt, ...);
extern void *kmalloc(size_t size);
extern void  kfree(void *ptr);
extern int   driver_register(driver_t *drv);
extern void  driver_unregister(driver_t *drv);

/* 模块入口约定：每个 .kdr 必须导出 kdr_init / kdr_exit */
typedef int  (*kdr_init_fn)(void);
typedef void (*kdr_exit_fn)(void);

/* ---- 内核侧接口（驱动/设备管理器）---- */
int  device_register(device_t *dev);
void device_unregister(device_t *dev);
void driver_manager_init(void);
void device_manager_init(void);
void device_manager_scan_pci(void);
void device_manager_add_demo(uint32_t id, const char *name);

/* 两个管理器内部协作入口（不暴露给模块） */
driver_t *driver_list_head(void);
void device_try_bind_driver(driver_t *drv);     /* 让 drv 尝试绑定所有未绑设备 */
void device_try_bind_all_drivers(device_t *dev); /* 让 dev 尝试匹配所有驱动 */

/* 从引导模块加载所有 .kdr 内核驱动（kdr_init 注册 driver_t）。
 * 由内核在第三步(系统初始化)按 registry 配置门控后调用。 */
int kdr_load_all(void);

#endif /* _SUKI_SUKIOS_KDR_H */
