# Step 21：P0-6 UEFI 启动路径（OVMF + GRUB-EFI + GOP 帧缓冲 + MB2 RSDP 通路）

## 一、总览与实测结果

P0-6 目标（step10_todos 原文）：「UEFI 启动路径：OVMF + GOP 帧缓冲」。

交付后同一 `kernel.elf`、同一 ISO 在两种固件形态下全链路启动：

```
# make run-uefi-headless（OVMF）
[boot] display: framebuffer (graphics)          <- OVMF GOP 帧缓冲
[boot] firmware: UEFI (OVMF/GOP path)
[acpi] RSDP from Multiboot2 tag (UEFI-safe path)
[acpi] RSDP found @ 0xffff800000008518 (revision=2)   <- ACPI 2.0 / XSDT
[acpi] XSDT @ 0x000000007fb7d0e8: 5 tables (FACP APIC HPET WAET BGRT)
[smp] 4/4 CPUs online / IPI selftest 3/3
[security] SMEP=on SMAP=on UMIP=on NXE=on
[ahci] port 0 online: 131072 sectors, LBA0 sig OK(55AA), wr-test PASS
[shell] SukiOS shell online

# make run-headless（SeaBIOS，回归）
[boot] firmware: Legacy BIOS
[acpi] RSDP from Multiboot2 tag (UEFI-safe path)      <- BIOS 下 GRUB 也给 tag
[acpi] RSDP found @ ... (revision=0)                  <- ACPI 1.0 / RSDT
全部 selftest PASS，shell 上线，零回归。
```

## 二、架构决策：为什么内核几乎零改动

GRUB-EFI 的 multiboot2 loader 行为：内核 MB2 头**不含** EFI boot services
标签时，GRUB 自动调用 `ExitBootServices()`，关分页、降到 32 位保护模式，
以 `EAX=0x36D76289, EBX=MBI 物理地址` 跳入内核入口——与 BIOS 路径完全一致。
因此：

- `boot/boot.S` 零改动（32 位入口、临时页表、长模式切换全部复用）；
- `boot/multiboot2_header.S` 零改动（tag5 帧缓冲请求对 GOP 同样有效）；
- 帧缓冲：OVMF GOP 提供线性帧缓冲，GRUB 经 MB2 tag8 转交，内核 `fb_init`
  无感知差异（同为物理地址+pitch+bpp 的直接 RGB 模式）。

UEFI 与 BIOS 的**真实差异**只有两处，即本批的全部工作面：

1. **RSDP 位置**：UEFI 下 RSDP 挂在 EFI 配置表（任意物理页，本次实测
   0x8518 附近的副本/0x7FBxxxxx 区域），EBDA/0xE0000 传统扫描必然落空；
2. **无 VGA 文本模式**：UEFI 没有 0xB8000，若 GRUB 不转交帧缓冲，内核
   "VGA 文本回退"实际是对着不存在的硬件写——屏幕全黑（本批实测踩到并修复）。

## 三、代码改动明细

### 3.1 include/kernel/multiboot2.h
- 新增 tag 常量：`MULTIBOOT_TAG_TYPE_EFI64=12`（EFI64 系统表指针，作 UEFI
  启动标识）、`ACPI_OLD=14`（ACPI 1.0 RSDP 副本 20B）、`ACPI_NEW=15`
  （ACPI 2.0+ RSDP 副本 36B，含 XSDT 指针）。
- `boot_info_t` 新增：`bool efi_boot`、`uint64_t rsdp_copy_phys`。

### 3.2 kernel/arch/x86_64/multiboot2.c
- 解析 tag12：置 `efi_boot=true`。EFI 系统表指针本身暂不消费（Boot
  Services 已被 GRUB 终结；Runtime Services 需 SetVirtualAddressMap
  重映射，列入 P1）。
- 解析 tag14/15：记录 RSDP 副本**物理地址**（`mbi_phys + tag内偏移+8`）。
  tag15（2.0+，含 XSDT）优先，出现即覆盖 tag14。

### 3.3 kernel/acpi/acpi.c + include/kernel/acpi.h —— RSDP 三级定位
- 新增 `acpi_set_rsdp_hint(uint64_t phys)`（kmain 在 acpi_init 前调用）。
- `acpi_init` 定位顺序改为：
  0. **MB2 副本**（提示非 0 时）：先验签名 "RSD PTR " + 校验和（rev0 验
     20 字节，rev2+ 验 36 字节）——不信任引导器数据，验失败落到 1)；
  1. EBDA 前 1KiB 扫描（BDA 0x40:0x0E 段基址 ×16）；
  2. 固定区 0xE0000..0xFFFFF 扫描（16 字节步长）。
- BIOS 机型 0) 同样命中（GRUB-BIOS 也注入 tag14），1)/2) 成为纯兜底。

### 3.4 grub/grub.cfg —— GOP 帧缓冲关键修复
```
insmod all_video
set gfxpayload=1024x768x32
```
踩坑实录：首轮 UEFI 启动 `display: VGA text (fallback)`——GRUB-EFI 默认
不加载视频驱动，无法满足内核 MB2 头的 tag5 帧缓冲请求，于是 MBI 里没有
tag8，内核回退"VGA 文本"（UEFI 下即黑屏，仅串口可见）。`insmod all_video`
让 GRUB 自动挑选 efi_gop（UEFI）/vbe（BIOS）后端，修复后两路均为
`framebuffer (graphics)`。

### 3.5 kernel/kmain.c
- `acpi_set_rsdp_hint(g_boot.rsdp_copy_phys)`（acpi_init 之前）；
- 启动日志新增 `[boot] firmware: UEFI (OVMF/GOP path) / Legacy BIOS`。

### 3.6 Makefile —— OVMF 运行目标
```make
OVMF_CODE := /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS := $(BUILD)/OVMF_VARS.fd          # 每次从只读模板复制
QEMU_UEFI := -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
             -drive if=pflash,format=raw,file=$(OVMF_VARS)

run-uefi / run-uefi-headless: $(ISO) $(DISK) $(OVMF_VARS)
    $(QEMU) $(QEMU_FLAGS) $(QEMU_UEFI) ... -boot d -cdrom $(ISO) $(QEMU_AHCI_DISK)
```
- pflash 双闪存：CODE 只读挂载，VARS 用 build/ 下可写副本（EFI 变量落副本，
  模板保持干净）；
- UEFI 目标刻意挂 **AHCI** 盘（真实 UEFI 机器几乎必为 AHCI/NVMe），同时
  回归验证 P0-7 驱动在 UEFI 环境的中断路由；
- ISO 无需任何改动：`grub-mkrescue` 检测到 /usr/lib/grub/x86_64-efi 模块后
  产的就是 BIOS+UEFI 混合 El Torito 镜像（xorriso 报告确认两个 boot img：
  `1 BIOS /boot/grub/i386-pc/eltorito.img`、`2 UEFI /efi.img`）。

## 四、验证方式

```bash
make iso
xorriso -indev build/SukiOS.iso -report_el_torito plain   # 确认双引导项
make run-uefi-headless    # OVMF：firmware: UEFI + GOP framebuffer + XSDT
make run-headless         # SeaBIOS 回归：firmware: Legacy BIOS
make run-ahci-headless    # AHCI 回归
```
验收点（全部实测通过）：
1. UEFI 下 `firmware: UEFI` + `display: framebuffer`（GOP 1024x768x32）；
2. `[acpi] RSDP from Multiboot2 tag` + revision=2 + XSDT 5 表（含 BGRT/WAET
   ——OVMF 特有表，侧证确实在读 UEFI 固件的 ACPI）；
3. SMP 4/4 + IPI 3/3（OVMF 的 MADT 布局与 SeaBIOS 不同，AP 启动无回归）；
4. AHCI 中断路由 + 写测试 PASS（UEFI 环境 GSI 路由正确）；
5. Ring3 shell 上线（syscall/IPC/调度全链路）；
6. BIOS 路径零回归（RSDP 走 tag 副本 rev=0/RSDT，一切 selftest PASS）。

## 五、遗留（列入 P1）

- EFI Runtime Services 接入（需 SetVirtualAddressMap 或恒等映射运行时区）；
- ESP 直启（自制 EFI stub 取代 GRUB）与内核代码段 KASLR（PIE + 重定位）；
- BGRT 开机 Logo 平滑接管。

## 六、P0 阶段完结

| 项 | 文档 |
|---|---|
| P0-1 ACPI | step13~15 |
| P0-2 LAPIC/IOAPIC | step13~15 |
| P0-4 高精度时钟 | step13~15 |
| P0-9 panic 诊断 | step16 |
| P0-3 SMP | step17 |
| P0-5 按需分页+COW+mmap | step18 |
| P0-7 AHCI | step19 |
| P0-8 安全地基 | step20 |
| **P0-6 UEFI 启动** | **本批（step21）** |

**P0 全部 9 项完成。**
