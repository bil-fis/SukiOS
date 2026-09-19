# step96 —— Python 构建配置器（类 Linux menuconfig）+ `*-with-cfg` 目标

## 0. 需求与最终设计
用户要求：创建 Python 构建配置器（内核能力 / 内核版本号 / 内核信息 / 需要编译的驱动等），
并明确：**只有 `make run-with-cfg`（及同类 `*-with-cfg`）使用配置文件；其余既有 make 指令一律保持全量编译**，
且**不改动现有 make 指令**。

最终设计：
- 新增配置器 `tools/kbuild_config.py` + schema/值文件；新增 `config/menuconfig/defconfig/listconfig` 目标。
- 新增 **`*-with-cfg`** 目标（`run-with-cfg` / `run-with-cfg-headless` / `iso-with-cfg` / `disk-with-cfg`）：
  以子 make 方式传 `WITH_CFG=1` + 独立构建目录 `build/cfg/`，据此**裁剪驱动、注入内核信息**。
- **既有目标（iso/iso-single/disk/run/run-headless/...）默认 `WITH_CFG=0`：永不裁剪、永远全量编译**，
  仅“内核信息/版本横幅”取自配置。

## 1. 新增/改动文件
| 文件 | 说明 |
|---|---|
| `configs/build/kconfig.json` | 配置 schema（分节 + 选项：id/type/prompt/default/gate） |
| `configs/build/kconfig.conf` | 配置值（默认全量；随仓库提交） |
| `tools/kbuild_config.py` | 配置器：`menuconfig`/`defconfig`/`listconfig`/`genconfig [--quiet] [--outdir DIR]`/`get ID` |
| `include/kernel/kbuild_info.h` | 生成宏兜底默认；**全量构建时强制驱动为开** |
| `Makefile` | `WITH_CFG` 开关、`-DSUKI_WITH_CFG`、配置头注入、cfg 源裁剪、`*-with-cfg` 目标、blob 规则泛化 |
| `kernel/kmain.c` | 打印内核信息横幅；`usb_init()` 由 `CONFIG_DRIVER_USB` 门控 |
| 生成物（gitignore） | `build/config/build_config.{h,mk}`（全量）、`build/cfg/config/build_config.{h,mk}`（cfg） |

## 2. 配置项
- **内核信息**：`KERNEL_NAME` / `KERNEL_VERSION` / `KERNEL_CODENAME` / `KERNEL_BUILD_INFO`。
- **内核能力**：`CONFIG_CAP_KASLR` / `CONFIG_CAP_PREEMPT` / `CONFIG_CAP_SMEP_SMAP` / `CONFIG_CAP_DEBUGFS`。
- **设备驱动**：`CONFIG_DRIVER_USB`（已接入源裁剪）、`HDA` / `AHCI` / `ATA` / `E1000` / `PS2`。

## 3. Makefile 机制
1. `WITH_CFG ?= 0`；`-include $(BUILDCFG_MK)`，且在**解析期**用 `$(shell genconfig --outdir $(BUILDCFG_DIR))`
   生成配置头（必须先于 `C_SRCS` 裁剪与 `CFLAGS` 展开，否则 `-include` 只解析一次会读到旧值）。
2. 内核 `CFLAGS/ASFLAGS` 追加 `-include $(BUILDCFG_H)` 与 `-DSUKI_WITH_CFG=$(WITH_CFG)`。
3. **源裁剪仅当 `WITH_CFG=1`**：
   ```
   ifeq ($(WITH_CFG),1)
   ifneq ($(CONFIG_DRIVER_USB),y)
   C_SRCS := $(filter-out $(shell find kernel/drivers/usb -name '*.c'),$(C_SRCS))
   endif
   endif
   ```
4. `include/kernel/kbuild_info.h`：当 `SUKI_WITH_CFG != 1`（全量）时 `#undef`/`#define` 所有
   `CONFIG_DRIVER_*` 为 1 —— 因此即使配置文件把某驱动设为 `n`，全量构建照旧包含它。
5. 新增目标：
   ```
   CFG_BUILD   := build/cfg
   CFG_SUBMAKE := $(MAKE) WITH_CFG=1 BUILD=$(CFG_BUILD)
   run-with-cfg:           @$(CFG_SUBMAKE) run
   run-with-cfg-headless:  @$(CFG_SUBMAKE) run-headless
   iso-with-cfg:           @$(CFG_SUBMAKE) iso-single
   disk-with-cfg:          @$(CFG_SUBMAKE) disk
   ```
   子 make 用独立 `build/cfg/`，与全量对象互不干扰。

## 4. 关键修复：blob 规则对 `BUILD` 覆盖的健壮性
原 `$(BUILD)/user/%.ssvc.blob.o` 规则把 objcopy 符号前缀写死为 `_binary_build_user_...`，
只匹配 `BUILD=build`；当 `BUILD=build/cfg` 时 objcopy 实际生成 `_binary_build_cfg_user_...`，
导致 `user_<name>_start/end` 未定义、内核链接失败。
修复：按输入路径动态推导前缀
```
_blob_pfx = _binary_$(subst /,_,$(subst .,_,$(subst -,_,$<)))
```
（`/ . -` 一律替换为 `_`；`build/user/x.elf` 与 `build/cfg/user/x.elf` 均正确）。Rust blob 规则同法修复。

## 5. 验证
（1）**全量构建不受配置影响**：把 `CONFIG_DRIVER_USB=n` 后 `make iso-single`
→ 仍编译 USB 源、`nm build/kernel.ski` 含 `usb_init`（横幅仍为 `SukiOS 1.0.0 'Hybrid'`）。

（2）**`*-with-cfg` 才裁剪**：`make iso-with-cfg`（`WITH_CFG=1`，`BUILD=build/cfg`）
→ `usb 源编译数=0`、`nm build/cfg/kernel.ski | grep usb_init = 0`；启动 `build/cfg/SukiOS-single.iso`
→ **USB 日志=0**、横幅存在、`shell online`、零 panic。

（3）**接线**：`make -n run-with-cfg` → `make WITH_CFG=1 BUILD=build/cfg run`。

（4）**既有命令不受影响**：`make iso-single` / `make disk` / `make listconfig` 均正常；
带 USB 与不带 USB 两场景零 panic。

（5）配置器 CLI：`defconfig` 生成默认全量；`listconfig` 打印全部选项；`genconfig --outdir` 定向输出。

## 6. 使用方式
```
make menuconfig        # 交互式配置（保存到 configs/build/kconfig.conf）
make defconfig         # 恢复默认（全量）
make listconfig        # 查看当前配置

make run-with-cfg      # 用配置文件构建并运行（其余 run/iso/disk 仍全量）
make run-with-cfg-headless
make iso-with-cfg      # 用配置文件产出 ISO（build/cfg/SukiOS-single.iso）
make disk-with-cfg
```

## 7. 提交
`025023f feat(build): Python 构建配置器（类 menuconfig）+ 配置注入与驱动裁剪`
（本轮 `*-with-cfg` 语义、`build/cfg` 目录、blob 规则泛化为后续提交）
