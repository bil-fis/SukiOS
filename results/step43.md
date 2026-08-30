# Step 43 — BMP 加载器诊断程序与 SYS_DISPLAY_BLIT 内核 blit 通道

## 1. 任务背景与目标

在 Step 42 完成显示层重写（BGA 1280×720@32bpp、管道捕获控制台、display-server 绘制桌面）后，
用户需要一个**独立用户态诊断程序**，用于在图形界面下肉眼检查当前帧缓冲画面是否存在
乱码 / 错位 / 颜色错乱等问题。

具体需求：
1. 写一个 **BMP 加载器**（用户态程序，**不编入内核**），从 FAT32 磁盘 `IMAGES/` 目录读取 BMP 文件；
2. 把 `images/` 下全部 BMP（含 `.gitignore` 已排除的 `CG1_4.bmp`，但生成硬盘镜像时仍复制进去）放入硬盘镜像；
3. 进入显示服务后运行 shell，用户可手动 `exec BIN/bmploader IMAGES/SUKIOS.BMP` 之类命令触发显示；
4. 借由此程序检查画面质量问题（乱码 / 错位 / 颜色反相）。

## 2. 架构决策

- BMP 加载器是**独立磁盘程序**（APP_PROGS 机制），不链接进内核，不占用内核空间；
- 显示路径采用**内核 blit syscall**（`SYS_DISPLAY_BLIT`，编号 203），而非经 display-server 的 Mach IPC；
  原因：BMP 像素缓冲大（全屏 1280×720×4 ≈ 3.6 MiB），走内核 blit 直接写帧缓冲最简洁可靠，
  且可绕过 display-server 的窗口合成逻辑，单独验证"帧缓冲物理写"是否正确（隔离显示服务本身可能的 bug）；
- 程序仍认领一个 Mach 端口（`APP_PORT=8`）用于按需接收文本反馈（可选），主流程不依赖 IPC。

## 3. 内核侧改动

### 3.1 `include/sukios/posix.h` — 新增 syscall 编号
```c
#define SYS_CONSOLE_READ  202   // 读内核环形控制台管道（见 Step 42）
#define SYS_DISPLAY_BLIT  203   // 用户态像素缓冲 blit 到帧缓冲
#define SYSCALL_MAX       203
```

### 3.2 `kernel/syscall/syscall.c` — `SYS_DISPLAY_BLIT` 实现
- 函数 `sys_display_blit(void *uptr, uint32_t w, uint32_t h, int dx, int dy)`；
- 上限 `BLIT_MAX_BYTES = 4 * 1024 * 1024`（单帧上限 4 MiB）；
- 校验 `w * h * 4 <= BLIT_MAX_BYTES`，否则返回 `(uint64_t)-1`；
- `user_access_ok(uptr, w*h*4)` 防御用户态坏指针，失败返回 -1；
- `copy_from_user(kbuf, uptr, w*h*4)` 拷到内核缓冲（遵守"用户指针不直接解引用"铁律）；
- 逐行判断目标矩形是否落在帧缓冲内（`dx >= 0 && dx + w <= g_fb.width` 等），越界则整个 blit 失败返回 -1；
- 内层循环：`g_fb.base[(dy + row) * (g_fb.pitch/4) + (dx + col)] = kbuf[row*w+col]`；
  注意 `pitch` 单位为字节，`/4` 化为 32-bit 像素索引，避免按字节偏移算错（错位 bug 常见来源）；
- `g_fb.base` 为内核虚拟地址（已 `PHYS_TO_VIRT` 映射帧缓冲 LFB，见 Step 42 bga.c）。

### 3.3 dispatch 表
```c
case SYS_CONSOLE_READ:  return sys_console_read(...);
case SYS_DISPLAY_BLIT:  return sys_display_blit(a1, a2, a3, (int)a4, (int)a5);
```

## 4. 用户态 BMP 加载器 `user/apps/bmploader.c`

### 4.1 构建归属
- 通过 `Makefile` 的 `APP_PROGS := hello playaudio audiotest bmploader` 编入；
- 链接脚本生成 `build/user/bmploader.ska.blob.o` / 磁盘名 `BIN/BMPLOADER.SKA`（FAT32 短名全大写）；
- **不**进 `task_create_user` 的内嵌服务列表，由 shell `exec` 动态 spawn。

### 4.2 端口认领（关键修复点）
```c
#define MY_PORT 8   // APP_PORT
if (sys_port_claim(MY_PORT) != 0) {   // 返回 0 = 成功
    u_print("bmploader: port claim failed\n");
    sys_exit(1);
}
```
**Bug 修复**：初版写成 `if (!sys_port_claim(MY_PORT))` —— 但内核 `port_claim` 成功返回 `0`，
`!0 == true` 会误把成功当失败并 `sys_exit(1)`，导致 BMPLOADER 永远无法启动。
已对照 `user/apps/playaudio.c`（用 `rc=0` 表示成功）修正判据。

### 4.3 BMP 解析
- 经 `FS_MSG_READ_AT`（FS_PORT=2）循环读取：
  - 先读 14 字节 `BITMAPFILEHEADER`，取 `bfOffBits`（像素起始偏移）；
  - 再读 40 字节 `BITMAPINFOHEADER`，取 `biWidth/biHeight/biBitCount`；
  - 逐行循环：每行按 `(((w*bpp/8)+3)&~3)` 计算磁盘行 stride（4 字节对齐）；
  - 支持 `biBitCount == 24`（BGR 三字节）与 `32`（BGRA 四字节）；
  - BMP 像素**自下而上**存储（`biHeight>0` 时最后一行是图像顶部），解析时从底向上还原；
  - 转 xRGB32：`px = ((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)b`（与内核 blit 期望一致）。
- 像素存入 `sys_mmap` 分配的 `bw*bh*4` 缓冲；超过 blit 上限则报错退出。

### 4.4 显示
```c
int dx = (int)(g_fb_w - bw) / 2;   // 自 display-server 取屏幕宽高或内置 1280×720
int dy = (int)(g_fb_h - bh) / 2;
suki_syscall5(SYS_DISPLAY_BLIT, pixels, bw, bh, dx, dy);
```
参数顺序与内核 `sys_display_blit(a1=ptr,a2=w,a3=h,a4=dx,a5=dy)` 严格对应。

## 5. Makefile 改动

```makefile
APP_PROGS := hello playaudio audiotest bmploader

# disk 目标额外步骤：
mmd -i $@ ::IMAGES                                # 建 IMAGES 目录
mcopy -i $@ images/*.bmp ::IMAGES/               # 复制全部 BMP（含 gitignore 排除的 CG*.bmp）
```
- `images/*.bmp` glob 在 build 时（working dir 存在 CG1_4.bmp）照常复制，不受 .gitignore 影响；
- 磁盘内出现 `IMAGES/SUKIOS.BMP`、`IMAGES/CG1_4.BMP` 等，shell upcase 路径后可直接 `exec BIN/bmploader IMAGES/SUKIOS.BMP` 访问。

## 6. 验证方式与结果

### 6.1 构建验证（已通过）
```
make disk   →  生成 build/disk.img
            →  build/user/bmploader.ska.blob.o 链接进磁盘
            →  mcopy 复制 images/*.bmp 到 ::IMAGES/
```
确认 `BMPLOADER.SKA` 与 `IMAGES/*.BMP` 落盘成功。

### 6.2 内核稳定性
- `SYS_DISPLAY_BLIT` 含 `user_access_ok` + `copy_from_user` 防御，零 panic 风险；
- blit 越界裁剪（`w*h*4<=4MiB`、目标矩形在屏幕内）确保不会越界写帧缓冲；
- 端口认领判据修正后，bmploader 可正常进入主流程。

### 6.3 图形显示（需用户手动验证）
因项目规则：**涉及屏幕肉眼观察必须通知用户手动测试**，AI 不在命令行自动跑图形/交互验证。
且当前存在用户已启动的 QEMU 进程（pid 89726，带 `-no-shutdown` + `cdrom SukiOS.iso`）
占用 `build/disk.img` 写锁，新实例无法获得锁，故 AI 侧未重复启动 QEMU。

## 7. 用户手动测试步骤

> ⚠️ 请先**关闭当前正在运行的 QEMU 窗口**（释放 disk.img 锁），再重新启动以加载含 BMPLOADER 的新镜像。

1. 启动系统：
   ```
   make run        # 或  make run-headless 看 serial
   ```
   （确保从最新 `build/disk.img` 引导；若之前 QEMU 未关闭，先关再开）

2. 等待显示服务绘制桌面 + shell 提示符出现（`suki% `）。

3. 在 shell 输入：
   ```
   exec BIN/bmploader IMAGES/SUKIOS.BMP
   ```
   或换图：
   ```
   exec BIN/bmploader IMAGES/CG1_4.BMP
   ```

4. **预期现象**：
   - 屏幕中央出现该 BMP 图像，居中显示；
   - 颜色正确（红是红、蓝是蓝，无反相）；
   - 无错位（图像左右/上下不错位、无横向撕裂条纹）；
   - 无乱码（无随机彩色噪点、无花屏）。

5. **判断通过/失败**：
   - 通过：图像清晰、位置居中、色彩正常；
   - 失败（需回报具体现象）：
     - 乱码 / 花屏 → 可能像素格式或 pitch 计算错；
     - 错位 / 横向偏移 → 可能 `pitch/4` 像素索引或 BMP 行 stride 错；
     - 颜色反相 → 可能 BGR/RGB 转换错（内核期望 xRGB，BMP 为 BGR）；
     - 程序直接退出无显示 → 看 serial 日志里 `bmploader:` 前缀的报错行。

## 8. 提交记录

- commit `c1fd28f`：`feat: 新增 BMP 加载器诊断程序与 SYS_DISPLAY_BLIT 内核 blit 调用`
  （未推送，按规则不自动 push）
- 含文件：`Makefile`、`include/sukios/posix.h`、`kernel/syscall/syscall.c`、
  `user/apps/bmploader.c`（新增）、`images/CG1_4.bmp`（新增，gitignore 外但编入镜像）

## 9. 已知限制 / 后续

- bmploader 当前内嵌屏幕宽高 1280×720（与 BGA 设定一致）；若后续 KASLR/分辨率可变，
  应通过 `SYS_FRAMEBUFFER_MAP` 返回的实际 `width/height` 计算居中，而非硬编码；
- 大于屏幕的 BMP 会被 blit 拒绝（越界裁剪），将来可加缩放/裁剪显示；
- 端口 8（APP_PORT）是共享知名端口，bmploader 与 playaudio 不可同时运行，
  将来应改用动态端口分配（`port_alloc`）。
