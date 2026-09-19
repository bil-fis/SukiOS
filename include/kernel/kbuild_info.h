/*
 * include/kernel/kbuild_info.h
 * -----------------------------------------------------------------------------
 * 构建配置器（tools/kbuild_config.py）生成值（build/config/build_config.h，
 * 经 -include 注入每个内核编译单元）的兜底默认。
 *
 * 若生成头存在，其 #define 先于此生效，本文件的 #ifndef 不会覆盖；
 * 若生成头缺失（如未构建/无 python），则用此处默认——且默认全量（驱动/CONFIG 均 1），
 * 保证「现有 make 指令依然全量编译」。
 */
#ifndef _SUKI_KERNEL_KBUILD_INFO_H
#define _SUKI_KERNEL_KBUILD_INFO_H

/* 内核信息/版本 */
#ifndef KERNEL_NAME
#define KERNEL_NAME "SukiOS"
#endif
#ifndef KERNEL_VERSION
#define KERNEL_VERSION "1.0.0"
#endif
#ifndef KERNEL_CODENAME
#define KERNEL_CODENAME "Hybrid"
#endif
#ifndef KERNEL_BUILD_INFO
#define KERNEL_BUILD_INFO "SukiOS hybrid kernel (xnu-like, x86_64)"
#endif

/* 内核能力（默认开） */
#ifndef CONFIG_CAP_KASLR
#define CONFIG_CAP_KASLR 1
#endif
#ifndef CONFIG_CAP_PREEMPT
#define CONFIG_CAP_PREEMPT 1
#endif
#ifndef CONFIG_CAP_SMEP_SMAP
#define CONFIG_CAP_SMEP_SMAP 1
#endif
#ifndef CONFIG_CAP_DEBUGFS
#define CONFIG_CAP_DEBUGFS 1
#endif

/* 驱动开关（默认全量开） */
#ifndef CONFIG_DRIVER_USB
#define CONFIG_DRIVER_USB 1
#endif
#ifndef CONFIG_DRIVER_HDA
#define CONFIG_DRIVER_HDA 1
#endif
#ifndef CONFIG_DRIVER_AHCI
#define CONFIG_DRIVER_AHCI 1
#endif
#ifndef CONFIG_DRIVER_ATA
#define CONFIG_DRIVER_ATA 1
#endif
#ifndef CONFIG_DRIVER_E1000
#define CONFIG_DRIVER_E1000 1
#endif
#ifndef CONFIG_DRIVER_PS2
#define CONFIG_DRIVER_PS2 1
#endif

/*
 * 全量构建（默认；SUKI_WITH_CFG != 1）：强制所有驱动为开，忽略配置文件里的
 * 驱动开关。只有 WITH_CFG=1 的 *-with-cfg 目标才会按生成头里的 CONFIG_DRIVER_*
 * 裁剪（配合 Makefile 的源目录过滤）。
 */
#if !defined(SUKI_WITH_CFG) || (SUKI_WITH_CFG == 0)
#undef  CONFIG_DRIVER_USB
#define CONFIG_DRIVER_USB 1
#undef  CONFIG_DRIVER_HDA
#define CONFIG_DRIVER_HDA 1
#undef  CONFIG_DRIVER_AHCI
#define CONFIG_DRIVER_AHCI 1
#undef  CONFIG_DRIVER_ATA
#define CONFIG_DRIVER_ATA 1
#undef  CONFIG_DRIVER_E1000
#define CONFIG_DRIVER_E1000 1
#undef  CONFIG_DRIVER_PS2
#define CONFIG_DRIVER_PS2 1
#endif

#endif /* _SUKI_KERNEL_KBUILD_INFO_H */
