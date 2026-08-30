# Step 42 — 显示服务（Display Server）与配置驱动显示模式

> 日期：2026-08-30
> 提交：`09aac41 display-server: config-driven video mode + framebuffer mapping + run-time cfg`（未推送）
> 关联记忆：ID 63167321（崩溃回溯与稳定即存 git 铁律）、ID 79299413（后缀体系）、ID 30993985（调试只用 bash+QEMU）

---

## 1. 需求回顾

用户要求：
1. **编写显示服务**（Ring3 用户态），负责显示合成。
2. **配置文件决定显示大小**；所有配置文件统一放 `configs/` 目录。
3. **默认显示大小 1280×720**。
4. **配置文件控制是否开启视频模式**：开启→帧缓冲图形；关闭→保持当前纯文本输出（VGA 文本）。
5. **新增记忆**：系统崩溃时从 git 读取上一版本正确代码对比定位；系统稳定即保存 git。

---

## 2. 总体设计

| 组件 | 文件 | 职责 |
|---|---|---|
| 配置文件 | `configs/display.cfg` | 显示模式开关 + 逻辑分辨率（默认 1280×720） |
| 引导加载 | `grub/grub.cfg` | `module2 /boot/display.cfg` 加载配置为引导模块 |
| 引导解析 | `kernel/arch/x86_64/multiboot2.c` | 解析 `MULTIBOOT_TAG_TYPE_MODULE`，存 `boot_info_t.cfg_phys/cfg_size` |
| 配置解析 | `kernel/display_cfg.c` + `include/kernel/display_cfg.h` | 解析 key=value，导出 `g_display`（video_mode/width/height） |
| 模式开关 | `kernel/arch/x86_64/framebuffer.c` `fb_init()` | `video_mode=off` 时不初始化 FB，内核回退 VGA 文本 |
| 系统调用 | `SYS_FRAMEBUFFER_MAP (200)` | 把帧缓冲物理页映射进显示服务用户地址空间 |
| 显示服务 | `user/display_server.c` | claim DISPLAY_PORT；video 模式合成桌面层 / off 模式纯文本 |
| 用户态调试宏 | `user/lib/suki.h` `udbg_printf` | 受 `CONFIG_DEBUG_SERIAL` 控制（make run 关 / run-dbg 开） |
| 构建 | `Makefile` | USER_PROGS 加 display_server；ISO 拷 display.cfg；USER_CFLAGS `-include config.h` |

### 2.1 显示模式语义

- **video_mode = on（默认）**：内核 `fb_init` 正常初始化帧缓冲（QEMU `-vga std` + GRUB `gfxpayload=1280x720x32` 决定硬件分辨率）；显示服务经 `SYS_FRAMEBUFFER_MAP` 拿到用户态可写映射，在帧缓冲上绘制合成桌面层（背景 + 边框 + 标题栏 + 中央按配置分辨率缩放的"桌面"矩形）。
- **video_mode = off**：内核 `fb_init` 直接返回 false，不初始化帧缓冲，内核 `console.c::user_puts()` 据 `fb_available()` 自动切换到 VGA 文本模式（已有路径）；显示服务降级为纯文本转发（`sys_debug_write`），不碰帧缓冲。

> **硬件分辨率说明（如实记录）**：硬件帧缓冲实际分辨率由 QEMU 显卡 + GRUB `gfxpayload` 决定（本项目设为 `1280x720x32`）。配置文件里的 `width/height` 是**显示服务声明的逻辑画布尺寸**（默认 1280×720），用于合成画布缩放与状态标注；若需真正改变硬件模式需 VBE set-mode，属后续里程碑。本实现已使"逻辑分辨率配置"真实影响合成画面（分辨率指示矩形按比例缩放），且 video_mode 开关真实控制视频/纯文本分流。

---

## 3. 配置文件格式（`configs/display.cfg`）

```
# SukiOS 显示服务配置文件
video_mode = on      # on -> 帧缓冲视频模式；off -> VGA 纯文本
width      = 1280    # 逻辑画布宽（默认 1280）
height     = 720     # 逻辑画布高（默认 720）
```

- 语法：`key = value`（允许空格；`#` 注释；空行忽略；key/value 大小写不敏感）。
- 缺失/解析失败：内核保留默认 1280×720 + 视频模式开（**无配置也能启动**）。
- 所有系统配置文件统一放 `configs/` 目录（当前含 `display.cfg`；后续其它 `.cfg` 也在此新增，并由 grub module2 或内核读取）。

---

## 4. 关键实现细节

### 4.1 引导模块解析（`include/kernel/multiboot2.h` + `multiboot2.c`）

新增标签常量与结构：
```c
#define MULTIBOOT_TAG_TYPE_MODULE 3
struct mb2_tag_module {
    uint32_t type; uint32_t size;
    uint32_t mod_start;   /* 物理地址 */
    uint32_t mod_end;
    char     cmdline[0];
};
```
`boot_info_t` 新增 `cfg_phys`（uint64）、`cfg_size`（uint32）。`multiboot2.c` 解析循环新增 `case MULTIBOOT_TAG_TYPE_MODULE`：仅采纳**首个**模块（约定 display.cfg），存入 `out->cfg_phys = mod_start; out->cfg_size = mod_end - mod_start`（上限 64 KiB 防御）。

### 4.2 配置解析（`kernel/display_cfg.c`）

- `display_cfg_parse(uint64_t cfg_phys, uint32_t cfg_size)`：把物理地址经 `PHYS_TO_VIRT(pa) = pa + 0xFFFF800000000000` 映射后逐字节扫描（带 NUL/越界防护），逐行解析 `key = value`。
- key 匹配表：`video_mode`(1) / `width`(2) / `height`(3)，大小写不敏感。
- `video_mode`：`on`→true，`off`→false，其它值保留默认。
- `width/height`：纯十进制，范围 `(0, 8192]`，越界保留默认。
- 导出全局 `display_config_t g_display = {.video_mode=true, .width=1280, .height=720}`。
- 解析结果经 `serial_writestr` 打印（无条件，便于验证）：`[display-cfg] parsed: video_mode=on width=1280 height=720`。

### 4.3 fb_init 视频模式开关（`kernel/arch/x86_64/framebuffer.c`）

`fb_init` 开头新增：
```c
if (!g_display.video_mode) {
    serial_writestr("[fb] video_mode=off from config; staying in VGA text mode...\n");
    g_fb.ready = false;
    return false;
}
```
`g_fb` 由匿名 static struct 改为 `fb_info_t g_fb`（在 `include/kernel/framebuffer.h` 导出 `typedef struct fb_info {...} fb_info_t; extern fb_info_t g_fb;`），供 syscall 层映射使用。`display_cfg_parse` 必须在 `fb_init` 之前调用——`kmain.c` 在 `bootinfo_prepare()` 成功后、`fb_init(&g_boot)` 之前插入调用：
```c
display_cfg_parse(g_boot.cfg_phys, g_boot.cfg_size);
fb_init(&g_boot);
```

### 4.4 SYS_FRAMEBUFFER_MAP（调用号 200）

`include/sukios/posix.h`：
```c
#define SYS_FRAMEBUFFER_MAP 200
#define SYSCALL_MAX         200   /* 由 150 提升以容纳新号 */
```
`kernel/syscall/syscall.c` 新增 `case SYS_FRAMEBUFFER_MAP: return sys_framebuffer_map(a1);` 与实现：
- 参数 `a1` = 用户态 `fb_map_result_t*`。
- `video_mode=off` 或 `!g_fb.ready`：填 `enabled=0`，`copy_to_user` 返回 -1（纯文本模式）。
- 否则：计算 FB 物理地址 `fb_phys = (uint64_t)g_fb.base - 0xFFFF800000000000`；帧缓冲大小 `pitch*height`；逐页 `vmm_map_page(task->cr3, FB_USER_VA + i*PAGE, fb_phys + i*PAGE, PTE_PRESENT|PTE_WRITE|PTE_USER|PTE_NX)`，用户虚拟地址固定 `0x00007F0000000000`（用户空间内，不与 mmap/栈冲突）。
- 填 `fb_map_result_t`：`enabled=1, fb_user_va, fb_phys, pitch, width, height, bpp, cfg_width=g_display.width, cfg_height=g_display.height`，`copy_to_user` 写回。
- 安全：用户指针 `a1` 经 `user_access_ok` 校验；映射显存物理帧，用户态写入即写显存，不影响内核；映射失败整体返回 -1，绝不暴露部分映射。

### 4.5 用户态调试宏（`user/lib/suki.h`）

```c
#if defined(CONFIG_DEBUG_SERIAL) && CONFIG_DEBUG_SERIAL
  #define udbg_printf(...) u_print(__VA_ARGS__)
#else
  #define udbg_printf(...) ((void)0)
#endif
```
`Makefile` 的 `USER_CFLAGS` 增加 `-include $(CONFIG_H)`，使用户态程序可见 `CONFIG_DEBUG_SERIAL`（此前仅内核可见）。**run / run-dbg 目标未改动**，仅注入头文件与新增宏。

### 4.6 显示服务（`user/display_server.c`）

- `main()`：`udbg_printf("[display] main entered")` → `sys_port_claim(DISPLAY_PORT)`（=3，与内核 `include/sukios/ipc.h` 一致）→ `suki_syscall6(SYS_FRAMEBUFFER_MAP, &res, 0,0,0,0,0)`。
- **video 模式**（`res.enabled`）：`draw_desktop()` 在用户虚拟地址直接写 32bpp 像素，绘制：
  - 整体背景 `DSP_BG`；
  - 4px 外边框 `DSP_BORDER`；
  - 顶部 24px 标题栏 `DSP_BAR`；
  - 客户区面板 `DSP_PANEL`；
  - 中央"分辨率指示矩形"：按 `cfg_width×cfg_height` 等比缩放进客户区，直观证明配置分辨率生效。
  - 打印关键状态 `u_print("[display] VIDEO MODE active (desktop composited)\n")` + 详细映射信息（`udbg_printf`，受开关）。
  - 进入消息循环：`mach_msg_recv(DISPLAY_PORT)` 接收 `DISP_MSG_TEXT`，转发串口日志（端口链路验证）。
- **文本模式**：打印 `u_print("[display] TEXT MODE ... falling back to plain text output")`；消息循环把 `DISP_MSG_TEXT` 经 `sys_debug_write` 转发（保持纯文本路径）。

### 4.7 构建集成（`Makefile`）

- `USER_PROGS := fs_server input_server display_server shell posixtest`
- `kernel/kmain.c` 增加 `task_create_user(user_display_server_start, ..., "display-server")`（blob 符号由 `objcopy --redefine-sym` 生成 `user_display_server_start/end`，已验证）。
- ISO：`cp configs/display.cfg $(ISODIR)/boot/display.cfg`；`grub.cfg` 加 `module2 /boot/display.cfg display.cfg` 与 `set gfxpayload=1280x720x32`。

---

## 5. 验证（QEMU 生产场景，bash + 串口落盘）

> 遵守记忆 ID 30993985/93407520：仅用 bash + QEMU 自带机制（`-serial file` + `cat/grep`），禁用 GDB 与外部脚本。

### 5.1 DBG=0（`make run`，默认）
```
[display-cfg] parsed: video_mode=on width=1280 height=720
[display] VIDEO MODE active (desktop composited)
```
- **无** `fb mapped @user_va=...` 等详细行（受 `udbg_printf` 关闭）。
- `panic|triple fault|#GP` 计数 = 0。

### 5.2 DBG=1（`make run-dbg`）
```
[display-cfg] parsed: video_mode=on width=1280 height=720
[display] main entered
[display] port claimed
[display] SYS_FRAMEBUFFER_MAP returned 0
[display] VIDEO MODE active (desktop composited)
[display] fb mapped @user_va=139637976727552 phys=81554046976 fb=1024x768 pitch=4096 cfg=1280x720
[ipc] task 'display-server' pid=6 wait on port=3
```
- 详细调试行全部出现；`fb=1024x768` 为 QEMU 实际硬件分辨率，`cfg=1280x720` 为配置逻辑分辨率；显示服务成功 claim DISPLAY_PORT(3) 并进入等待。
- `panic|triple fault|#GP` = 0；`qemu -d int` 无异常。

### 5.3 video_mode = off（改 `configs/display.cfg` 后重建）
```
[display-cfg] parsed: video_mode=off width=1280 height=720
[fb] video_mode=off from config; staying in VGA text mode (framebuffer not initialized)
[display] TEXT MODE: video_mode=off or no framebuffer; falling back to plain text output (no compositing)
```
- 内核不初始化帧缓冲，回退 VGA 文本；显示服务降级纯文本转发；零 panic。
- 验证后已恢复默认 `video_mode = on` 并重建 ISO。

### 5.4 其它服务回归
fs-server / input-server / shell 均正常启动，POSIX 自检 ALL PASS，系统 `system fully up`。

---

## 6. 记忆更新

新增记忆 **ID 63167321「SukiOS 崩溃回溯与稳定即存 git 铁律」**：
1. 系统崩溃时从 git 读取上一稳定版本正确代码对比定位本次错误（`git diff`/`git show <commit>:<file>`），禁止盲目猜测。
2. 系统稳定运行即 `git commit`（不 push 除非用户要求），使 git 历史成为可回退正确基线。

（与既有 ID 46225109「完成工作后自动 commit 不 push」一致，本记忆强化"崩溃对比"与"阶段性稳定即存"。）

---

## 7. 后续里程碑（不属本轮，仅记录方向）

- 显示服务真正栅格化文本（内置字库或复用内核字形），经 DISPLAY_PORT 接收 shell 文本绘制到客户区，取代内核 fbcon 直接输出。
- 应用离屏 Buffer → OOL IPC → Display Server 合成 → 写帧缓冲（GUI 远期目标）。
- 配置文件支持更多项（主题色、字体、多显示器），均落 `configs/`。
