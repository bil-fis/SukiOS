/*
 * include/kernel/bootanim.h
 * -----------------------------------------------------------------------------
 * 启动动画（启动画面 + 进度条）。
 *
 * 需求（用户）：
 *   - 进度条：黑底、白色填充、两侧圆角；由 System/Boot/ShowProgress 控制显隐；
 *   - 启动图标：由 System/Boot/ShowLogo 控制显隐；
 *   - System/Boot/BootLogoID 指定展示哪张内嵌启动图（如 "sukios_boot_temp_ver"）；
 *   - System/Boot/CustomLogo/Enabled=true 时改从 CustomLogo/Path（走 VFS）或
 *     CustomLogo/DirectPath（直接指定驱动器）加载。
 *
 * 内嵌启动图来源：boot/anim 下的 .bmp 由构建期 tools/bootanim_gen.py 转成
 * 「自描述原始位图」并用 objcopy 链入内核；符号表 build/bootanim/table.c
 * 由同一脚本生成（因此新增启动图只需把 bmp 丢进 boot/anim，无需改构建脚本）。
 *
 * 转换后 blob 布局（小端）：
 *   [0..4)   magic = BOOTANIM_MAGIC
 *   [4..8)   u32 width
 *   [8..12)  u32 height
 *   [12..16) u32 stride（像素，恒等于 width）
 *   [16..)   像素 u32 XRGB = (r<<16)|(g<<8)|b —— 与 32bpp 帧缓冲小端写入格式一致
 */
#ifndef _SUKI_KERNEL_BOOTANIM_H
#define _SUKI_KERNEL_BOOTANIM_H

#include <kernel/types.h>
#include <kernel/registry.h>   /* system_config_t */

#define BOOTANIM_MAGIC 0x494E4153u   /* 小端 'S''A''N''I'（Suki ANImation） */

/* 内嵌启动图条目（构建期生成，见 build/bootanim/table.c）。 */
typedef struct {
    const char    *name;    /* 标识：bmp 文件名去扩展名，对应注册表 BootLogoID */
    const uint8_t *start;
    const uint8_t *end;
} bootanim_blob_t;

/* 以 name == NULL 结尾的符号表；无启动图时仅含结尾哨兵。 */
extern const bootanim_blob_t g_bootanim_blobs[];

/*
 * bootanim_setup —— 绘制启动画面（由 boot_late_init 在 registry 解析后调用）。
 *   - 清屏为黑色；
 *   - ShowLogo    为真时绘制启动图标（CustomLogo 优先，其次 BootLogoID 内嵌图）；
 *   - ShowProgress 为真时绘制进度条（黑底白填充、两侧圆角）并把进度归零。
 * 帧缓冲不可用时静默返回（纯文本回退模式），绝不阻塞启动。
 */
void bootanim_setup(const system_config_t *cfg);

/*
 * bootanim_progress —— 推进进度条到 pct%（0..100，单调不减）。
 * ShowProgress 为假、未初始化或帧缓冲不可用时为空操作。
 */
void bootanim_progress(uint32_t pct);

/*
 * bootanim_finish —— 启动画面收尾（在拉起显示服务之前调用）。
 * 保证启动画面至少可见 BOOTANIM_MIN_MS，期间把进度平滑补到 100%，然后返回；
 * 随后显示服务的合成才会覆盖启动画面。
 * 必要性：单核实测从绘制启动画面到显示服务接管帧缓冲不足 0.5 秒，若不保持，
 * 进度条会一闪而过。帧缓冲不可用（纯文本回退）时立即返回，不阻塞启动。
 */
void bootanim_finish(void);

/*
 * bootanim_handoff / bootanim_boot_done —— 启动序列完成的「交接闸门」。
 *
 * 语义：内核在启动早期持有屏幕绘制开机动画；显示服务映射帧缓冲并调用
 * SYS_DISPLAY_READY 之后，会阻塞在 SYS_BOOT_SPLASH_WAIT（内部轮询
 * bootanim_boot_done()）上，直到内核在【驱动 + 全部 Ring3 服务就绪、启动画面
 * 收尾完毕】后调用 bootanim_handoff() 放行 —— 此后显示服务才绘制桌面、进入
 * 消息循环，用户登录/桌面流程随即开始。
 *
 * 若显示服务未映射帧缓冲（纯文本回退）则无人等待，handoff 只是置位标志。
 */
void bootanim_handoff(void);
bool bootanim_boot_done(void);

#endif /* _SUKI_KERNEL_BOOTANIM_H */
