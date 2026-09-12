/*
 * kdrv/example_kdrv.c
 * -----------------------------------------------------------------------------
 * 示例内核模块（.kdr）：演示「一套驱动驱动多个设备」。
 *
 * 该模块用交叉 gcc 编成 ET_DYN 共享对象（见 Makefile 的 KDR 规则），由内核 kdr
 * 加载器在启动时加载。模块导出 kdr_init()，在其中注册一个 driver_t：
 *   - example_match()：匹配「虚拟总线 + 类 0x80 / 子类 0x01」的设备；
 *   - example_probe()：对每个匹配设备分配独立 drvdata（dev->drvdata）；
 *   - 内核在启动时注册【两个】该型虚拟设备，于是本驱动被 probe 两次，
 *     driver->nbound 累加为 2 —— 即一套驱动服务多个设备。
 *
 * 模块调用的 kprintf / kmalloc / kfree / driver_register 均为内核经
 * EXPORT_SYMBOL 导出的符号，由加载器在重定位时解析（模块自身不静态链接内核）。
 */
#include <sukios/kdr.h>

static bool example_match(const device_t *dev)
{
    return dev->bus == BUS_VIRTUAL &&
           dev->class_code == 0x80 &&
           dev->subclass   == 0x01;
}

static int example_probe(device_t *dev)
{
    /* 为每个设备分配独立私有数据：同一份驱动函数服务多个设备实例 */
    dev->drvdata = kmalloc(32);
    if (!dev->drvdata)
        return -1;
    kprintf("[example_kdrv] probe '%s' (id #%u) drvdata=%p\n",
            dev->name ? dev->name : "?", dev->dev_id, dev->drvdata);
    return 0;
}

static void example_remove(device_t *dev)
{
    kprintf("[example_kdrv] remove '%s' (id #%u)\n",
            dev->name ? dev->name : "?", dev->dev_id);
    kfree(dev->drvdata);
    dev->drvdata = NULL;
}

static driver_t g_example_drv = {
    .name   = "example-kdrv",
    .match  = example_match,
    .probe  = example_probe,
    .remove = example_remove,
};

int kdr_init(void)
{
    kprintf("[example_kdrv] init: registering driver '%s'\n", g_example_drv.name);
    driver_register(&g_example_drv);
    return 0;
}

void kdr_exit(void)
{
    kprintf("[example_kdrv] exit\n");
    driver_unregister(&g_example_drv);
}
