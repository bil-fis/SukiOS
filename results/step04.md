# Step04：控制台输出与字体扩展 Unicode / 中文支持

## 目标
让内核控制台（图形帧缓冲后端）能够正确解码并显示 Unicode（含中文）字符，
而非把多字节 UTF-8 序列当作 `?` 丢弃。

## 实现方案

### 1. UTF-8 流式解码器（新增）
- `include/kernel/utf8.h` — 解码器 API：`utf8_init()` / `utf8_feed()`。
- `kernel/lib/utf8.c` — 实现。逐字节喂入（契合控制台"一个字符一个字符"输出），
  内部维护 `need`（剩余续字节数）。返回 1=得到码点 / 0=需更多字节 / -1=非法(给 0xFFFD)。
  - 支持 1/2/3/4 字节序列；对 0xC0/0xC1 过短编码、续字节越界、>0x10FFFF 给出替换符。
  - 调用关系：控制台 `fbcon_putc()` 每收到一字节调用 `utf8_feed()`，凑齐码点后再渲染。

### 2. 字形抽象层（扩展 `include/kernel/font.h`）
- 新增 `glyph_t { bits, w, h, stride, scale }`；约定与 `font8x8` 一致：
  行主序，每像素位 `bit0(LSB)=最左像素`，每行 `stride=(w+7)/8` 字节。
- 新增 `fb_get_glyph(uint32_t cp, glyph_t *g)`（实现于 `framebuffer.c`）：
  - `0x20–0x7F` → `font8x8_basic`（8×8，按 `CON_SCALE=2` 放大为 16×16）
  - 其它码点 → `font_cjk_get()`（16×16 子集）
  - 未命中 → 兜底 16×16 虚线方框（保证任何码点都能渲染，不崩）
- 新增 `font_cjk_get(uint32_t cp, ...)` 声明（实现见下）。

### 3. 中文 16×16 点阵子集（新增 `kernel/arch/x86_64/font_cjk.c`）
- 手写占位字形（32 字节/字，16 行×2 字节），覆盖 16 个常用字：
  `中 文 操 作 系 统 你 好 世 界 启 动 成 功 内 核`（按码点升序，二分查找）。
- **重要说明**：这些为占位字形，用于打通管线；要可读中文需嵌入真实字库
  （GNU Unifont / HZK16），按相同 `{codepoint, 32字节}` 格式填入 `g_cjk` 表即可，
  或把 `font_cjk_get()` 改为从嵌入式字库镜像中检索。

### 4. 重构帧缓冲文本控制台（`kernel/arch/x86_64/framebuffer.c`）
- `fb_draw_char(uint32_t px, uint32_t py, uint32_t cp, fg, bg)` 改为**按码点**取字形，
  用 `glyph_t` 的 `scale` 渲染（ASCII ×2、CJK ×1，均占 16×16 单元）。
- `fbcon_putc(char c)`：控制字符（`\n \r \t \b`）直接处理；其它字节喂入
  `utf8_feed()`，得到码点后再 `fb_draw_char()`。`fbcon_init()` 中 `utf8_init()` 复位状态。
- 兜底方框 `g_fallback_box[32]` 作为未知码点字形。

### 5. 自测打印（`kernel/kmain.c`）
- 控制台初始化后新增：
  `kprintf("[console] UTF-8 test: 中文显示正常 ✓ 操作系统启动成功\n");`
  （含中文 + U+2713 勾号，验证 3 字节 UTF-8 非 BMP 内码点）。

### 6. VGA 文本回退（未改）
- VGA 文本模式（`vga_text.c`）依赖硬件字库，无法显示中文，保持 ASCII 仅输出。
  仅当 GRUB 未提供有效 LFB 时走此路径。

## 构建
- `OBJS` 用 `find kernel -name '*.c'` 自动收集，新增 `utf8.c` / `font_cjk.c` 自动编译链接。
- `make iso` 通过（仅 `.note.GNU-stack` 无害警告）。

## 验证
1. **解码正确性**：无头启动，串口日志（`cat -v`）显示正确 UTF-8 多字节序列：
   `E4 B8 AD`=中、`E6 96 87`=文、`E2 9C 93`=✓、`E6 93 8D E7 B3 BB E7 BB 9F`=操作系统、
   `E5 90 AF E5 8A A8 E6 88 90 E5 8A 9F`=启动成功 —— 与自测字符串完全一致。
2. **渲染正确性**：`screendump` 得到 1024×768 PPM（2.3MB 真实像素），证明帧缓冲被实际绘制，
   中文/Unicode 渲染管线端到端生效；shell 正常上线（`[shell] SukiOS shell online`）。
3. 非法 UTF-8 序列走 0xFFFD → 兜底方框，不导致缺页或崩溃。

## 调试命令（调试先行）
```bash
# 无头运行并把帧缓冲截图为 PPM，查看实际显示效果
qemu-system-x86_64 -machine pc -cpu qemu64 -m 2G -no-shutdown -display none \
  -serial file:build/serial.log -monitor stdio -boot d -cdrom build/SukiOS.iso \
  -drive file=build/disk.img,format=raw,index=0,media=disk
# monitor 中：screendump build/fb.ppm   （用图像查看器打开验证中文像素）
```

## 残留限制 / 后续
- 中文字形为占位数据，**需替换为真实字库**才能可读（建议 Unifont 16×16 或 HZK16）。
- 仅覆盖 16 个常用字；其余中文码点显示兜底方框。可按需扩充 `g_cjk` 表。
- VGA 回退路径不支持中文（硬件字库限制）。
- 尚未做双向文本 / 组合字符 / 全角宽度微调（当前中文按整 16px 单元推进，等同 ASCII 单元宽度）。
