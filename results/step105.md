# step105 —— 注册表反编译工作流 + 内核注册表读取接口 + 启动动画（图标/圆角进度条）+ 启动设备标识

## 0. 需求（用户）

1. 用 Python 注册表管理器把现有注册表反编译成 JSON，审核后修改 JSON 再编译回 `.sre`；
   `system/boot` 下新增 `BootDeviceType`（标记是否从光驱启动 / 判断哪个设备挂到 `/`）、
   `ShowProgress`（是否显示进度条）、`ShowLogo`（是否显示启动图标）、`BootLogoID`（启动图标识）；
   `boot/anim/sukios_boot_temp_ver.bmp` 编译进内核并以 `sukios_boot_temp_ver` 作为标识；
   `System/Boot/CustomLogo=true` 时从该键下 `Path`（走 VFS）或 `DirectPath`（直接指定驱动器）加载；
   并把新增驱动等写入注册表。
2. 审核结论：`CustomLogo` 保持子键形态；`BootDeviceType` 只是注册表内的**标识**，实际由内核探测，
   用户态请求读取该值时由内核返回**真实结果**；JSON 不入库（由 `make desre` / `make csre` 手动互转）。
3. 进度条：黑底白填充，两侧圆角。

---

## 1. 注册表内容更新（审核后）

`configs/default/system.sre`：**799 → 3034 字节**。

| 位置 | 动作 | 值 |
|---|---|---|
| `System/Boot/BootDeviceType` | 改值 | `""` → `"auto"` |
| `System/Boot/ShowLogo` / `ShowProgress` | 保留 | `true` / `true` |
| `System/Boot/BootLogoID` | **新增**(string) | `"sukios_boot_temp_ver"` |
| `System/Boot/Verbose` | **新增**(bool) | `false`（补齐内核早已在读、hive 里却缺失的值） |
| `System/Boot/CustomLogo/Enabled|Path|DirectPath` | 保留 2 项 + **新增 DirectPath** | `false` / `""` / `""` |
| `System/Kernel/KdrEnabled` | **新增**(bool) | `true`（同上，补齐内核已在读的缺口） |
| `System/Drivers/<厂商>/<驱动>` | **替换占位键** | 10 个真实驱动（Intel×6 / Bochs×1 / Generic×2 / QEMU×1），每项 `DisplayName/Bus/Match/Builtin/Path/Enabled` |
| `System/Services` | 补全 | 原 Display/Fs/Input + **Net/Mouse/Shell/Console**（`services.sre` 同步） |

驱动清单（`Bus/Match` 取自实际匹配代码 `kernel/driver/builtin_drivers.c`）：

| 厂商/驱动 | Bus | Match | Builtin | 说明 |
|---|---|---|---|---|
| Intel/ATA-IDE | pci | `01/01` | ✔ | `ata.c`（BMIDE DMA + ATAPI 光驱） |
| Intel/AHCI | pci | `01/06` | ✔ | `ahci.c` |
| Intel/IntelHDA | pci | `04/03` | ✔ | `hda.c` |
| Intel/E1000 | pci | `02/00` | ✔ | `e1000.c` |
| Intel/UHCI | pci | `0C/03` | ✔ | `usb/uhci.c` |
| Intel/I8042-PS2 | platform | `ps2` | ✔ | 键鼠 |
| Bochs/VGA | pci | `03/00` | ✔ | BGA 帧缓冲 |
| Generic/USB-HID | usb | `03/*` | ✔ | `usb/usb_hid.c` |
| Generic/USB-Hub | usb | `09/*` | ✔ | `usb/usb_hub.c` |
| QEMU/DemoLED | platform | `demo` | ✘ | `Path=/SYS/DRIVERS/example_kdrv.kdr`（可加载 .kdr） |

### 1.1 反编译/编译工作流（JSON 不入库）

`Makefile` 新增（`.gitignore` 忽略 `configs/default/*.sre.json`）：

```bash
make desre   # .sre -> .sre.json（反编译，供人工编辑；别名 reg-export）
make csre    # .sre.json -> .sre（编译回二进制；别名 reg-compile）
```

**同时修复一个真实构建坑**：`.sre` 原先不是镜像目标的依赖，`make csre` 改了注册表后
`make iso/disk` 会「Nothing to be done」，跑的还是旧配置。现将
`REG_SRES` 列为 `$(DISK)` 与 `$(ISO_SINGLE)` 的依赖。

---

## 2. 内核注册表层

### 2.1 `include/kernel/registry.h`
- `system_config_t` 增加 Boot 段：`boot_device_type` / `show_logo` / `show_progress` /
  `boot_logo_id` / `custom_logo_{enabled,path,direct_path}`（定长字符数组，超长安全截断）。
- 新增：`RegistryCacheSystem()` / `RegistryQuery()` / `RegistrySetOverrideString()` /
  `RegistryOverrideLookup()`。

### 2.2 `kernel/registry/hive.c`
- `RegistryParseSystem()` 增补解析上述 Boot 键（含 `System/Boot/CustomLogo/{Enabled,Path,DirectPath}`）。
- **运行时缓存**：`RegistryCacheSystem()` 把整份 hive 字节 kmalloc 拷贝留下，
  此后任意路径查询都直接在内存里导航键树（不再碰磁盘）。
- **动态值覆盖**：`RegistrySetOverrideString(path, value)` 让「注册表内只是标识、
  真实值由内核探测」的键返回运行时结果；`RegistryQuery()` 优先返回覆盖值。
- 内核侧 `RegistrySelfTest()`（kmain）开机打印 10 条路径查询结果作为验收。

### 2.3 新增系统调用 `SYS_REGISTRY_READ (212)`

```c
a1 = 用户态路径（NUL 结尾，如 "System/Boot/ShowLogo"）
a2 = 输出缓冲, a3 = 容量, a4 = uint32_t* 回填类型（可空）
返回 写入字节数 / -1 路径不存在 / -2 参数非法
```

Ring3 读 `System/Boot/BootDeviceType` 拿到的是**内核实际探测结果**（`disk`/`cdrom`/`none`）。

另有 **`SYS_BOOT_SPLASH_WAIT (213)`**（启动画面交接闸门，见 §3.3），仅显示服务使用。

### 2.4 启动设备标识（`BootDeviceType`）
内核在 `boot_late_init` 的两个分支分别探测并回声：

```
[boot] BootDeviceType: detected 'disk'  (kernel probe), / = FAT32 disk
[boot] BootDeviceType: detected 'cdrom' (kernel probe), / = ISO9660 CD-ROM
```

随后 `RegistrySetOverrideString("System/Boot/BootDeviceType", ...)` 覆盖读值。

---

## 3. 启动动画

### 3.1 内嵌启动图（新增构建期打包）
- `tools/bootanim_gen.py`：扫描 `boot/anim` 下**全部** .bmp → 转成自描述原始位图
  （`SANI` magic + w/h/stride + XRGB32 像素）+ 生成 `blobs.S`（`.incbin` 链入 .rodata）
  与 `table.c`（`BootLogoID` → 符号区间 的符号表）。
- `Makefile`：`BOOTANIM_OBJS` 纳入内核链接。**新增启动图只需把 bmp 丢进 `boot/anim`**。
- `boot/anim/sukios_boot_temp_ver.bmp`（1280×720 24bpp，2.6 MiB）→ 3.5 MiB blob。

### 3.2 绘制（`kernel/boot/bootanim.c` + `include/kernel/bootanim.h`）
- `bootanim_setup(cfg)`：清屏为黑；`ShowLogo` 决定画不画图标；`ShowProgress` 决定画不画进度条。
- 图标来源优先级：`CustomLogo/Enabled`（`DirectPath` 优先，支持 `cd0:`/`disk0:` 前缀；
  否则 `Path` 走 VFS，失败回退内核 ISO9660）→ `BootLogoID` 内嵌图 → 内嵌表第一张。
- **铺满全屏**（`AnimBlitCover`，cover 语义）：等比缩放到覆盖整屏（`max` 缩放比），
  超出部分居中裁剪，**不拉伸变形**；16.16 定点最近邻（内核无 FPU/SSE），1:1 时走
  `memcpy` 快路径。自定义 BMP 支持 24/32bpp 无压缩、上下行序均支持。
- **进度条**（`AnimRoundRect`，两侧圆角=药丸形）：黑底轨道 + 白色填充，宽 = 屏宽/3、
  高 14px、位于屏幕 3/4 高度处居中，叠加绘制在启动图之上；`bootanim_progress(pct)`
  单调推进。
- `bootanim_finish()`：**最短展示时长 2000ms**，期间把进度平滑补到 100%。
  见 §5.1 的实测原因。

### 3.3 启动画面交接闸门（开机动画保持到全部初始化完成）

用户要求：开机动画必须一直显示到「系统驱动初始化 + 服务初始化」**全部**完成，
之后才离开动画界面进入后续流程（用户登录/桌面）。

- 新增 `bootanim_handoff()` / `bootanim_boot_done()`（`kernel/boot/bootanim.c`）。
- 新增系统调用 **`SYS_BOOT_SPLASH_WAIT (213)`**（仅显示服务调用）：
  轮询 `bootanim_boot_done()` + `msleep(5)` 真睡眠（不忙等、不空转 CPU）。
- 显示服务（`user/display_server.c`）改为：
  1. 映射帧缓冲 → 只画**离屏**背景层 `draw_desktop()`（不碰显存）；
  2. `SYS_DISPLAY_READY`（内核置 `g_display_active`，诊断改走环形管道，不再涂抹
     开机动画）；
  3. **阻塞在 `SYS_BOOT_SPLASH_WAIT`**；
  4. 返回后才 `composite()` 提交首帧桌面并进入消息循环。
- `boot_late_init` 顺序重排：
  `驱动 → 磁盘/FS/registry/kdr → 开机动画(30%) → registry 自检 →`
  **`显示服务(spawn → 等 ready → 阻塞在闸门) 40%`** `→ 网卡服务 45% → Ring3 网络服务 60%
  → 输入服务 70% → 鼠标服务 78% →` **`bootanim_finish() → bootanim_handoff()`** `→`
  `SukiLogon → SukiShell（进入登录/桌面）+ 各项自检`。

### 3.4 进度里程碑
`15%` 配置/kdr → `30%` 磁盘/FS → `40%` 显示服务就绪（仍持有屏幕）→ `45%` 网卡服务 →
`60%` 网络服务 → `70%` 输入服务 → `78%` 鼠标服务 → `bootanim_finish()` 100% →
`handoff` 交接给桌面。

---

## 4. 修复的 bug

### 4.1 【关键】ATAPI PIO 多块读：第 2 块起被读成全零
**现象**：仅光盘启动时 `[boot] config: hive parse/CRC failed, using defaults`
（配置 799B 时代正常，层级化 3034B 后必现）。

**定位**（QEMU 串口日志 + 主机侧比对）：
- 主机侧解析 ISO 内的 hive：magic/version/body_size/CRC **全部正确**；
- 内核诊断：`n=3034 magic=53554b5245470000 ver=2 body=2970 crc=7db8e422`（头部正确），
  但 `bodycrc` 与主机不符；逐偏移采样发现 `b2900` 主机非零、内核为零 → **body 尾部被清零**；
- 定位到 `kernel/drivers/cdrom.c: CdromPacket()`：数据相位把整段（`count*2048`）一口气
  搬完，**不在每个 2048 字节块前重新等待 DRQ**。第 2 块起在设备尚未就绪时读空 FIFO
  → 返回全零，且命令最终未置 ERR（`CdromReadBlocks` 的「全段非零」重试也察觉不到，
  因为第 1 块非零）。旧配置 403B/799B 均 < 2048B 只需 1 块，故缺陷长期潜伏。

**修复**：按块搬运，每块前 `CdromPoll(CdromCondDrq)` + 检查 ERR，单块 2048 字节。

### 4.2 注册表变更不触发镜像重建
`sre` 未列入 `$(DISK)` / `$(ISO_SINGLE)` 依赖 → `make csre` 后镜像不重建（见 §1.1）。

---

## 5. 验证（QEMU 生产场景，i440FX + Skylake-Client + 2G）

### 5.1 启动画面可见性（连拍 `screendump` + 程序化像素判定）
修复前：连拍 40 帧（0.35s 间隔）中**仅 1 帧**处于启动画面（<0.35s），进度条一闪而过。
修复后（`bootanim_finish` 最短 2000ms + 交接闸门）：启动画面从绘制起持续保持到
**全部服务就绪并放行**（连拍 30 帧 × 0.5s：f01–f05 为启动画面、f06 起为桌面），
进度条白色像素数 318 → 399 → 407 → 407 → 412 平滑增长，收尾为满格药丸形。

画面（抓屏）：启动图**铺满全屏**（`-> fullscreen 1280x720 (cover)`，四角文字
`sukios_boot_anim_temp` / `Ciallo～(∠・ω< )⌒★` / `开发中版本喵，非最终效果` /
`Development Version` 贴到屏幕四角，可见确实铺满）+ 黑轨道白填充圆角进度条；
放行后桌面接管（shell 终端窗口正常显示）。

### 5.2 磁盘启动
```
[boot] config: loaded system.sre (generation=1)
[boot] config: boot logo=on progress=on id='sukios_boot_temp_ver' custom=off
[boot] BootDeviceType: detected 'disk' (kernel probe), / = FAT32 disk
[boot] anim: logo 'sukios_boot_temp_ver' 1280x720 -> fullscreen 1280x720 (cover)
[boot] anim: progress bar 426x14 @ (427,540) r=7
[boot] display-server ready (g_display_active=1, waited=1 rounds); boot screen still held, mounting remaining services...
[boot] anim: boot screen done (held 2000 ms at least, progress=100%)
[boot] anim: handoff -> display server takes over (all drivers + services initialized)
[reg]   System/Boot/BootDeviceType -> str "disk"
[reg]   System/Boot/ShowLogo -> bool true
[reg]   System/Boot/BootLogoID -> str "sukios_boot_temp_ver"
[reg]   System/Kernel/KdrEnabled -> bool true
[reg]   System/Drivers/Intel/UHCI/Match -> str "0C/03"
[reg]   System/Services/Shell/Autostart -> bool true
[reg]   System/Display/Width -> int 1024
[reg]   System/Boot/NoSuchValue -> (missing)
[shell] SukiOS shell online (Ring3, bash-like)
```
零 panic/异常；`PASS=105 FAIL=0`；USB 键鼠/hub 正常枚举。

### 5.3 仅光盘启动（`make iso-single`）
`config: loaded system.sre` 恢复；`BootDeviceType -> "cdrom"`；自检 10/10 通过（修复 §4.1 后）。

### 5.4 CustomLogo 两条路径
- `Path=/images/sukios.bmp`（走 VFS）→ `anim: logo 'custom' 1466x1484 -> 355x360`
- `DirectPath=cd0:/images/sukios.bmp`（光驱直读，6.5MB BMP 同时压测 §4.1 的多块读）→ 同上

验证后已还原为 `Enabled=false`。

---

## 6. 关键文件与常量索引

| 类别 | 位置 |
|---|---|
| 注册表 JSON 工作流 | `Makefile`（`desre`/`csre`/`REG_SRES`）、`.gitignore` |
| 注册表数据 | `configs/default/{system,user,services}.sre` |
| 内核注册表 | `include/kernel/registry.h`、`kernel/registry/hive.c`、`SYS_REGISTRY_READ=212`（`include/sukios/posix.h`）、`kernel/syscall/syscall.c` |
| 启动动画 | `include/kernel/bootanim.h`、`kernel/boot/bootanim.c`、`kmain.c`（`draw_boot_logo`/`BootPlayAnimation`/`bootanim_progress`/`bootanim_finish`/`bootanim_handoff`/`RegistrySelfTest`） |
| 交接闸门 | `bootanim_handoff()`/`bootanim_boot_done()`、`SYS_BOOT_SPLASH_WAIT=213`（`include/sukios/posix.h`）、`kernel/syscall/syscall.c`（`sys_boot_splash_wait`）、`user/display_server.c`（先 ready 后阻塞再 composite） |
| 启动图打包 | `tools/bootanim_gen.py`、`boot/anim/`、`build/bootanim/{blobs.S,table.c}` |
| ATAPI 修复 | `kernel/drivers/cdrom.c`（`CdromPacket` 逐块 DRQ、`CD_BLOCK_BYTES`） |

常量：`BOOTANIM_MAGIC=0x494E4153`（`SANI`）、`ANIM_CUSTOM_MAX=16MiB`、`BOOTANIM_MIN_MS=2000`、
`SUKREG_LOGOID_MAX=64`、`SUKREG_LOGO_PATH_MAX=192`。

## 7. 遗留说明
- `System/Drivers` 本轮作为「驱动清单/使能声明」写入并由内核解析；**驱动注册发生在
  stage3 之前**，故暂不据其门控 probe（后续可把配置读取前移）。
- 显示服务接管帧缓冲（`g_display_active`）后启动画面即被桌面合成覆盖，符合设计。
- 仅光盘启动下 `PASS=102 FAIL=1`，唯一失败项为 `libc open create >= 0`：
  `/` 是 ISO9660 **只读**卷，创建文件必然失败，属该启动形态的固有差异（与本轮改动无关）；
  磁盘启动为 `PASS=105 FAIL=0`。
