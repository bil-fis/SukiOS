# =============================================================================
# SukiOS 顶层 Makefile
# -----------------------------------------------------------------------------
# 工具链自动探测：若系统已安装 SukiOS 专属交叉编译器 x86_64-sukios-elf-gcc 则优先
# 使用（由 make gcc 构建并装到 cross/opt/bin），否则回退到原生 gcc（配合
# -ffreestanding -nostdlib，x86_64 主机可正常产出自由环境内核）。安装专属交叉
# 工具链后无需改动本文件即可无缝切换。
# =============================================================================

ARCH := x86_64

# ---- 工具链安装前缀（与 cross/Makefile.gcc 的 PREFIX 保持一致）----
# cross/Makefile.gcc 中 CROSS_ROOT=$(CURDIR)、PREFIX=$(CROSS_ROOT)/opt，
# 故工具装在 $(CURDIR)/opt/bin。
CROSS_OPT := $(CURDIR)/opt

# ---- 工具链探测 ----
# 架构约定（重要）：
#   * 内核（kernel/*.c/.S）由【宿主原生 gcc】编译——内核是 freestanding、自带实现，
#     且需要宿主系统头文件（如 <string.h> 来自宿主 libc 头，内核仅用于编译期，
#     运行期不链接 libc）。绝不能用 x86_64-sukios-elf-gcc 编内核（它无宿主系统头）。
#   * 用户态程序（user/、apps/）由【SukiOS 专属交叉编译器 x86_64-sukios-elf-gcc】
#     编译，链接 SukiOS 自带 libc（user/lib），生成目标端 ELF。
# 因此：
#   CC      = 内核编译器 = 宿主 gcc（固定）
#   USER_CC = 用户态编译器 = 优先 x86_64-sukios-elf-gcc，未装则回退宿主 gcc
# 注意：Make 的 $(shell ...) 不继承 makefile 内 export 的 PATH，探测必须用绝对
# 路径 wildcard，而非依赖 command -v。
SUKIOS_CC := $(CROSS_OPT)/bin/x86_64-sukios-elf-gcc
ELF_CC    := $(CROSS_OPT)/bin/x86_64-elf-gcc
# 内核：固定宿主 gcc
CC        := gcc
TOOLCHAIN := native (host gcc, freestanding)
# 用户态：优先专属工具链，回退宿主 gcc
ifeq ($(wildcard $(SUKIOS_CC)),$(SUKIOS_CC))
  USER_CC := x86_64-sukios-elf-gcc
  USER_LD := x86_64-sukios-elf-ld
  USER_TOOLCHAIN := cross (x86_64-sukios-elf)
else ifeq ($(wildcard $(ELF_CC)),$(ELF_CC))
  USER_CC := x86_64-elf-gcc
  USER_LD := x86_64-elf-ld
  USER_TOOLCHAIN := cross (x86_64-elf)
else
  USER_CC := gcc
  USER_LD := ld
  USER_TOOLCHAIN := native (host gcc, freestanding)
endif
# 将专属工具链目录加入 PATH（供后续 recipe 中的 ld/as/objcopy 等被找到）
export PATH := $(CROSS_OPT)/bin:$(PATH)

# ---- 目录 ----
BUILD  := build
ISODIR := $(BUILD)/isodir
KERNEL := $(BUILD)/kernel.ski
ISO    := $(BUILD)/SukiOS.iso

# =============================================================================
# 内核编译期配置（include/kernel/config.h）
# -----------------------------------------------------------------------------
# SMP（对称多核）为【可选特性，默认关闭】：SukiOS 当前阶段的开发/验证主线
# 是单核（路径简单、可复现、无跨核竞态干扰），多核能力保留在代码树中，
# 需要时以 `make <target> SMP=1` 显式打开。
#
#   make iso            -> 单核镜像（CONFIG_SMP=0，QEMU -smp 1）
#   make iso SMP=1      -> 多核镜像（CONFIG_SMP=1，QEMU -smp 4）
#
# 影响面：
#   * CONFIG_SMP=1：MAX_CPUS=8，编入 AP 跳板(ap_boot.S)与 INIT-SIPI-SIPI
#     上线路径、per-CPU 运行队列 + work-stealing、IPI/TLB shootdown；
#   * CONFIG_SMP=0：MAX_CPUS=1（g_percpu[]/g_syscall_kstack[]/AP GDT/TSS/IST
#     等 per-CPU 数组全部收敛为单元素），ap_boot.S 从源文件列表剔除，
#     smp_init() 仅注册 IPI handler 并报告单核形态，不唤醒任何 AP。
#     （IPI_RESCHED handler 仍保留：单核下以 self-IPI 触发即时调度。）
# =============================================================================
SMP        ?= 0
CONFIG_SMP := $(if $(filter 1,$(SMP)),1,0)

# ---- 串口冗长诊断输出开关（CONFIG_DEBUG_SERIAL）----
# 默认 0：make run 产出的内核不含 IPC/disk-srv/console-srv 的逐条调试日志，
# 串口流量极小，系统全速运行（串口 PIO 输出极慢，完整 POSIX 层并发时每秒
# 数百条 IPC 若全打印会刷爆串口、严重拖慢系统）。
#   make run      -> CONFIG_DEBUG_SERIAL=0（默认）
#   make run-dbg  -> CONFIG_DEBUG_SERIAL=1，输出全部冗长诊断
# 经 build/config.h 注入到每个内核/用户编译单元（与 CONFIG_SMP 同机制）。
DBG                  ?= 0
CONFIG_DEBUG_SERIAL  := $(if $(filter 1,$(DBG)),1,0)

# ---- 运行超时（秒）----
# 默认 0：qemu 持续运行（前台需手动 Ctrl-C / 后台用 -no-shutdown 等信号退出）。
# 设为正整数时，用 coreutils `timeout` 包裹 qemu，到时自动 SIGTERM 退出，
# 便于自动化验证（无头跑 N 秒后落盘串口日志，再 grep 判断结果）。
# 例：make run-headless RUN_TIMEOUT=20
RUN_TIMEOUT         ?= 0

# 配置注入载体：由本 Makefile 生成，经 `-include` 插入到每个内核编译单元的
# 最前面。它被 -MMD 记录进 .d 依赖文件，因此 SMP 开关一改，全部 .o 自动
# 重编，绝不会出现「一半单核对象 + 一半多核对象」的混链。
CONFIG_H   := $(BUILD)/config.h

# ---- 编译/链接选项 ----
# 说明：手册 CFLAGS 原写 -mcmodel=kernel，但该模型要求内核位于顶部 2GB
# (0xFFFFFFFF80000000+)，与红线 KERNEL_BASE=0xFFFF800000000000 冲突，
# 故改用 -mcmodel=large（支持任意 64 位地址）。
# -MMD -MP：为每个 .o 生成 .d 头依赖文件并在末尾 include，保证修改 .h 后
# 所有包含它的 .c/.S 自动重编（曾因缺依赖跟踪导致 percpu_t 布局新旧混用崩溃）。
CFLAGS := -ffreestanding -nostdlib -std=gnu11 -Wall -Wextra -O2 \
          -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mgeneral-regs-only \
          -mcmodel=large -fno-pic -fno-pie -fstack-protector-strong -mstack-protector-guard=global \
          -fno-asynchronous-unwind-tables -fno-omit-frame-pointer \
          -MMD -MP -I include \
          -include $(CONFIG_H)

ASFLAGS := -ffreestanding -mcmodel=large -fno-pic -fno-pie -MMD -MP -I include \
           -include $(CONFIG_H)

# 注意（P0-8 KASLR）：--emit-relocs 只能用于【预链接】阶段（供 gen_relk.py
# 抽取 R_X86_64_64）。最终 kernel.elf 绝不能保留 .rela.* 节——GRUB multiboot2
# 装载器遇到带重定位节的 ELF 会直接拒绝：
#   "error: ELF files with relocs are not supported yet."
LDFLAGS := -nostdlib -static -no-pie -z max-page-size=0x1000 \
           -Wl,--build-id=none -T boot/linker.ld

# ---- 源文件 ----
# 注意：kernel/abilities/ 下是用户态能力库（如 miniz 压缩库，依赖 libc），
# 绝不能编进 freestanding 内核（链接会缺 stat/fopen 等符号）。显式排除该目录。
C_SRCS := $(shell find kernel -name '*.c' 2>/dev/null | grep -v '/abilities/')
S_SRCS := boot/boot.S boot/multiboot2_header.S boot/pvh.S $(shell find kernel -name '*.S' 2>/dev/null)

# 单核构建(CONFIG_SMP=0)：剔除 AP 启动跳板。跳板内符号(ap_tramp_start 等)
# 仅被 smp.c 的 #if CONFIG_SMP 分支引用，两者同步裁剪，链接必然自洽。
ifneq ($(CONFIG_SMP),1)
S_SRCS := $(filter-out kernel/arch/x86_64/ap_boot.S,$(S_SRCS))
endif

# ---- Ring3 系统服务（编译为 ELF，以字节流嵌入内核镜像，开机由内核直接装载） ----
USER_PROGS   := fs_server input_server display_server shell posixtest mouse_server net_server nettest dltest
USER_CFLAGS  := -ffreestanding -nostdlib -std=gnu11 -Wall -Wextra -O2 \
                -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mgeneral-regs-only \
                -mcmodel=small -fno-pic -fno-pie -fstack-protector-strong -mstack-protector-guard=global \
                -fno-asynchronous-unwind-tables -MMD -MP -I user -I include \
                -I user/lib/shims -include $(CONFIG_H)

# ---- lwIP 2.2.1（vendor 在 lib/） ----
# NO_SYS=1 下编译的核心/接口源文件（关闭 altcp/ipv6/igmp/autoip/socket）。
LWIP_DIR   := lib/lwip-2.2.1
LWIP_INC   := -I $(LWIP_DIR)/src/include -I user/lib
LWIP_SRCS  := $(LWIP_DIR)/src/core/def.c \
              $(LWIP_DIR)/src/core/inet_chksum.c \
              $(LWIP_DIR)/src/core/init.c \
              $(LWIP_DIR)/src/core/mem.c \
              $(LWIP_DIR)/src/core/memp.c \
              $(LWIP_DIR)/src/core/netif.c \
              $(LWIP_DIR)/src/core/pbuf.c \
              $(LWIP_DIR)/src/core/raw.c \
              $(LWIP_DIR)/src/core/stats.c \
              $(LWIP_DIR)/src/core/sys.c \
              $(LWIP_DIR)/src/core/tcp.c \
              $(LWIP_DIR)/src/core/tcp_in.c \
              $(LWIP_DIR)/src/core/tcp_out.c \
              $(LWIP_DIR)/src/core/timeouts.c \
              $(LWIP_DIR)/src/core/udp.c \
              $(LWIP_DIR)/src/core/ip.c \
              $(LWIP_DIR)/src/core/dns.c \
              $(LWIP_DIR)/src/core/ipv4/dhcp.c \
              $(LWIP_DIR)/src/core/ipv4/etharp.c \
              $(LWIP_DIR)/src/core/ipv4/icmp.c \
              $(LWIP_DIR)/src/core/ipv4/ip4.c \
              $(LWIP_DIR)/src/core/ipv4/ip4_addr.c \
              $(LWIP_DIR)/src/core/ipv4/ip4_frag.c \
              $(LWIP_DIR)/src/netif/ethernet.c
LWIP_OBJS  := $(patsubst $(LWIP_DIR)/%,$(BUILD)/lwip/%,$(LWIP_SRCS:.c=.c.o))

USER_LIB_OBJS := $(BUILD)/user/lib/crt0.S.o $(BUILD)/user/lib/suki.c.o \
                  $(BUILD)/user/lib/errno.c.o $(BUILD)/user/lib/string.c.o \
                  $(BUILD)/user/lib/stdlib.c.o $(BUILD)/user/lib/stdio.c.o \
                  $(BUILD)/user/lib/unistd.c.o $(BUILD)/user/lib/time.c.o \
                  $(BUILD)/user/lib/dirent.c.o \
                  $(BUILD)/user/lib/syscalls.c.o \
                  $(BUILD)/user/lib/pthread.c.o \
                  $(BUILD)/user/lib/signal.c.o \
                  $(BUILD)/user/lib/stack_canary.c.o \
                  $(BUILD)/user/lib/setjmp.S.o \
                 $(BUILD)/user/lib/suki_native.c.o \
                 $(BUILD)/user/lib/dlfcn.c.o

# FatFs（ChaN R0.16）核心：fs_server 用 FatFs 做 FAT32 解析，diskio.c 对接
# DISK_PORT IPC 做磁盘 IO。ff.c + ffunicode.c 编入 fs_server 的 blob/elf。
FATFS_SRCS := drivers/FatFs/ff.c drivers/FatFs/ffunicode.c drivers/FatFs/diskio.c
FATFS_OBJS := $(BUILD)/fatfs/ff.c.o $(BUILD)/fatfs/ffunicode.c.o $(BUILD)/fatfs/diskio.c.o
# 注意：ffsystem.c 在 FF_USE_LFN!=3 且 FF_FS_REENTRANT==0 时整文件被 #if 屏蔽，
# 故不编入；ff_memalloc/ff_memfree 由 user/lib/suki.c 的简易堆提供。

# 系统服务以 .ssvc 后缀作为内核内嵌 blob 的产物名（官方后缀：系统服务 → .ssvc）。
# 逻辑名（fs_server 等）保持不变，内核符号 user_fs_server_start 由 blob 规则用
# $(basename $*) 剥离 .ssvc 后缀得到，故 kmain.c 无需改动。
USER_BLOBS    := $(patsubst %,$(BUILD)/user/%.ssvc.blob.o,$(USER_PROGS))

# ---- 独立程序（standalone apps，源码在 user/apps/）----
# 这些程序不嵌入内核，仅放入 FAT32 磁盘的 ::BIN/ 目录（文件名无 .elf 后缀），
# 由 shell 经 exec/spawn 从磁盘装载。区别于系统服务：
#   * 可使用浮点 / SSE（minimp3 MP3 解码依赖），故启用 -msse2 且去掉
#     -mgeneral-regs-only（内核 switch.S 已 fxsave/fxrstor 保存 Ring3 SSE 上下文）。
#   * -Os 优先缩小体积，以适配内核 execve 单条 OOL(16 页=64KiB) 的加载上限。
APP_PROGS    := hello playaudio audiotest bmploader nettest dltest
APP_CFLAGS   := -ffreestanding -nostdlib -std=gnu11 -Os \
                -mno-red-zone -msse -msse2 \
                -ffunction-sections -fdata-sections \
                -mcmodel=small -fno-pic -fno-pie -fstack-protector-strong -mstack-protector-guard=global \
                -fno-asynchronous-unwind-tables -MMD -MP -I user -I include \
                -I user/lib/shims -I minimp3
APP_ELFS     := $(patsubst %,$(BUILD)/apps/%.elf,$(APP_PROGS))

# ---- 共享库 libtest.sl（动态链接 / dlopen 验证用）----
# 裸机交叉工具链的 ld 不支持 -shared（会退化为 ET_EXEC），故用 -pie 生成
# ET_DYN 位置无关对象（内核 elf_dlopen 按 ET_DYN 加载并重定位，等效共享库）。
# 显式构造 PIC 标志集（避免 USER_CFLAGS 的 -fno-pic/-fno-pie 残留），并关闭栈保护
# （共享库不链 stack_canary，避免 __stack_chk_fail 未定义）。
LIBTEST_SL     := $(BUILD)/libtest.sl
LIB_SL_CFLAGS  := -ffreestanding -nostdlib -std=gnu11 -Wall -Wextra -O2 \
                   -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mgeneral-regs-only \
                   -mcmodel=small -fPIC -fno-stack-protector \
                   -fno-asynchronous-unwind-tables -MMD -MP -I user -I include \
                   -I user/lib/shims -include $(CONFIG_H)
$(BUILD)/libs/libtest.c.o: user/libs/libtest.c
	@mkdir -p $(dir $@)
	$(USER_CC) $(LIB_SL_CFLAGS) -c $< -o $@
$(LIBTEST_SL): $(BUILD)/libs/libtest.c.o
	@mkdir -p $(dir $@)
	# 直接用 ld -shared 产出真正的 ET_DYN 共享对象（gcc -shared 在本交叉工具链会
	# 退化为 ET_EXEC，无法作为 -l 链接输入）。内核 elf_load 按 ET_DYN 处理。
	$(USER_LD) -shared -export-dynamic --no-warn-rwx-segments \
		--no-dynamic-linker -o $@ $<
	@echo "==> shared lib $@ ($$(stat -c%s $@) bytes)"

# ---- FreeType 静态库（字体服务 pchfnt/fontsrv 的字形光栅化引擎）----
# 仅编入 TrueType 渲染必需模块（base/sfnt/truetype/smooth/raster/autofit/
# psaux/pshinter/psnames），配合 user/lib/ftmodule_min.h 关闭其余驱动。
# 通过 FT_CONFIG_STANDARD_LIBRARY_H 指向 user/lib/freetype_shim.h 屏蔽宿主
# stdio 依赖（本仓仅用 FT_OPEN_MEMORY 从内存加载字体）。
FT_DIR       := lib/freetype-2.14.3
FT_SRC_DIRS  := base sfnt truetype smooth raster autofit psaux pshinter psnames
# 收集上述目录下的全部 .c（排除明显非编译单元，如工具/调试）
FT_SRCS      := $(foreach d,$(FT_SRC_DIRS),$(wildcard $(FT_DIR)/src/$(d)/*.c))
FT_OBJS      := $(patsubst $(FT_DIR)/%,$(BUILD)/ft/%,$(FT_SRCS:.c=.c.o))
FT_LIB       := $(BUILD)/libfreetype.a
FT_CFLAGS    := -ffreestanding -nostdlib -std=gnu11 -O2 -fno-asynchronous-unwind-tables -fcommon \
                -DFT2_BUILD_LIBRARY -DFT_CONFIG_OPTION_USE_ZLIB=0 \
                -DFT_CONFIG_STANDARD_LIBRARY_H='"user/lib/freetype_shim.h"' \
                -DFT_CONFIG_MODULES_H='"user/lib/ftmodule_min.h"' \
                -I $(CURDIR) -I $(FT_DIR)/include -I user/lib/shims -I user/lib -I include
# 单独编译 freetype 源（不依赖 USER_CFLAGS 的栈保护/red-zone 等，避免污染引擎）
$(BUILD)/ft/%.c.o: $(FT_DIR)/%.c
	@mkdir -p $(dir $@)
	$(USER_CC) $(FT_CFLAGS) -c $< -o $@

FT_SHIM_OBJ := $(BUILD)/user/lib/freetype_shim.c.o
$(FT_SHIM_OBJ): user/lib/freetype_shim.c
	@mkdir -p $(dir $@)
	$(USER_CC) $(FT_CFLAGS) -c $< -o $@

$(FT_LIB): $(FT_OBJS) $(FT_SHIM_OBJ)
	@mkdir -p $(dir $@)
	$(USER_CC) -nostdlib -r -Wl,--build-id=none -Wl,--allow-multiple-definition -o $@ \
		$(FT_OBJS) $(FT_SHIM_OBJ)
	@echo "==> FreeType static lib $(FT_LIB) ($(words $(FT_OBJS)) objects)"

# 字体相关独立程序：fontsrv（常驻字体服务）与 pchfnt（渲染命令行工具）
FONT_PROGS  := fontsrv pchfnt
FONT_ELFS   := $(patsubst %,$(BUILD)/apps/%.elf,$(FONT_PROGS))

# 字体程序编译时同样用 FT_CONFIG_STANDARD_LIBRARY_H 覆盖 ftstdlib.h，
# 并加入 freetype 公共头搜索路径；其余沿用 APP_CFLAGS（栈保护/SSE/用户态链接）。
FONT_CFLAGS := -DFT_CONFIG_STANDARD_LIBRARY_H='"user/lib/freetype_shim.h"' \
               -DFT_CONFIG_MODULES_H='"user/lib/ftmodule_min.h"' \
               -I $(CURDIR) -I $(FT_DIR)/include
$(BUILD)/apps/fontsrv.o: user/fontsrv.c
	@mkdir -p $(dir $@)
	$(USER_CC) $(APP_CFLAGS) $(FONT_CFLAGS) -c $< -o $@
$(BUILD)/apps/pchfnt.o: user/apps/pchfnt.c
	@mkdir -p $(dir $@)
	$(USER_CC) $(APP_CFLAGS) $(FONT_CFLAGS) -c $< -o $@


# Rust 示例 ELF（cargo build 生成）；不存在则留空，不影响其余程序构建。
# 注：RUST_DIR 在下方 rust 区段才定义，此处用相对项目根的字面前缀路径。
RUST_BIN := $(wildcard rust/target/x86_64-sukios/debug/sukios-hello)
# Rust 示例 ELF 的内嵌 blob（供内核开机自检 spawn；不存在则留空，weak 符号跳过）。
ifeq ($(RUST_BIN),)
RUST_BLOB :=
else
RUST_BLOB := $(BUILD)/user/rusthello.blob.o
endif

OBJS := $(patsubst %,$(BUILD)/%.o,$(C_SRCS) $(S_SRCS)) $(USER_BLOBS) $(RUST_BLOB)

# ---- 磁盘镜像 (FAT32) ----
DISK := $(BUILD)/disk.img

# ---- QEMU ----
# 注意：-machine pc (i440FX) 提供传统 IDE (PIIX3, PIO 0x1F0)；
#       q35 只有 AHCI，ATA PIO 驱动无法使用。
QEMU        := qemu-system-x86_64
# 运行超时包装：RUN_TIMEOUT=0（默认）时直接用 $(QEMU)；设为正整数时以
# coreutils `timeout` 包裹，到时自动 SIGTERM 退出（便于无头自动化验证）。
# 必须用 `=` 递归展开，确保引用到已定义的 $(QEMU)/$(RUN_TIMEOUT)。
ifeq ($(RUN_TIMEOUT),0)
QEMU_RUN    := $(QEMU)
else
QEMU_RUN    := timeout $(RUN_TIMEOUT) $(QEMU)
endif
# 注意：-no-reboot 会让 QEMU 把 8042 复位脉冲当作"关机"处理，配合
#       -no-shutdown 会进入 paused 状态而非真正重启。故移除 -no-reboot，
#       使内核的 sys_reboot()（8042 0xFE 脉冲）可触发真正的机器重启。
# KVM 自动检测：宿主机有 /dev/kvm 时启用硬件虚拟化（浮点/整数全部原生
# 执行，minimp3 解码可达实时数十倍；纯 TCG 仿真解码仅 ~1/8 实时，音频
# 必然断续）。可用 make run QEMU_ACCEL=tcg 强制回退软件仿真。
QEMU_ACCEL  ?= $(shell test -w /dev/kvm && echo kvm || echo tcg)
# P0-3 SMP：QEMU 的 CPU 数随【构建配置】联动——
#   多核构建(CONFIG_SMP=1)：默认 -smp 4，SeaBIOS 生成含 4 个 LAPIC 条目的
#     MADT，内核经 INIT-SIPI-SIPI 唤醒 3 个 AP；
#   单核构建(CONFIG_SMP=0，默认)：默认 -smp 1，与内核单核语义一致。
# 任何情况下都可用 make run QEMU_SMP=N 显式覆盖（例如单核镜像故意跑在
# 4 vCPU 上验证「不启动 AP」的行为）。
ifeq ($(CONFIG_SMP),1)
QEMU_SMP    ?= 4
else
QEMU_SMP    ?= 1
endif
ifeq ($(QEMU_ACCEL),kvm)
QEMU_FLAGS  := -machine pc,accel=kvm -cpu host -smp $(QEMU_SMP) -m 2G -no-shutdown
else
QEMU_FLAGS  := -machine pc -cpu qemu64 -smp $(QEMU_SMP) -m 2G -no-shutdown
endif
QEMU_DISK   := -drive file=$(DISK),format=raw,index=0,media=disk
# P0-7 AHCI：盘挂 AHCI 控制器（DMA+中断路径）而非 i440FX 传统 IDE。
# 用 make run-ahci / run-ahci-headless 验证 ahci.c；默认 run 仍走 IDE PIO。
QEMU_AHCI_DISK := -device ahci,id=myahci \
                  -drive file=$(DISK),format=raw,if=none,id=ahdisk0 \
                  -device ide-hd,drive=ahdisk0,bus=myahci.0
QEMU_SERIAL := -serial stdio

# ---- P0-6 UEFI：OVMF 固件（pflash 双闪存：CODE 只读 + VARS 可写副本） ----
# grub-mkrescue 产的 ISO 本身是 BIOS+UEFI 混合镜像（借助 /usr/lib/grub/
# x86_64-efi 模块生成 El Torito EFI 引导项），无需改 ISO 生成流程。
OVMF_CODE := /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS_SRC := /usr/share/OVMF/OVMF_VARS_4M.fd
OVMF_VARS := $(BUILD)/OVMF_VARS.fd
QEMU_UEFI := -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
             -drive if=pflash,format=raw,file=$(OVMF_VARS)

# ---- 音频：Intel HDA 控制器 (8086:2668, ICH6) + 输出编解码器 ----
# 我们的内核驱动通过 PCI class 0x04/subclass 0x03 探测该控制器。
# QEMU_AUDIODRV 可覆盖后端：pa(PulseAudio，WSLg/桌面默认)、alsa、sdl、
# 或 none(无声，仅供无宿主音频环境下验证 DMA 通路不崩)。
# 例：make run QEMU_AUDIODRV=alsa   或   make run-headless QEMU_AUDIODRV=none
QEMU_AUDIODRV ?= pa
QEMU_AUDIO  := -audiodev $(QEMU_AUDIODRV),id=snd0 \
               -device intel-hda -device hda-duplex,audiodev=snd0

# ---- 网络：Intel 82540EM (e1000) + user 模式后端 ----
# 内核驱动经 PCI class 0x02/subclass 0x00 探测该网卡（kernel/drivers/e1000.c）。
# QEMU user 后端自带虚拟网关/DHCP(10.0.2.2)、DNS(10.0.2.3)，驱动启动自检发的
# ARP 请求会得到应答，从而端到端验证 TX/RX 通路；后续 lwIP 亦可借此联网。
QEMU_NET    := -device e1000,netdev=net0 -netdev user,id=net0

.PHONY: all iso run run-headless run-dbg run-ahci run-ahci-headless run-uefi run-uefi-headless run-q run-q-debug debug clean info disk gcc make-rust-env rust-env rust-libs FORCE

all: $(KERNEL)
# 默认目标固定为 all：本 Makefile 中 FreeType/字体程序的规则位于 all 之前，
# 若不显式声明，make 会把第一个规则（freetype_shim.c.o）当成默认目标，
# 导致 `make` 只编一个对象就退出、内核镜像不重建。
.DEFAULT_GOAL := all

info:
	@echo "Toolchain : $(TOOLCHAIN)"
	@echo "CC        : $(CC)"
	@echo "SMP       : $(CONFIG_SMP) ($(if $(filter 1,$(CONFIG_SMP)),multi-core, single-core); make SMP=1 to enable)"
	@echo "DEBUG     : $(CONFIG_DEBUG_SERIAL) (serial verbose diag; make run-dbg to enable DBG=1)"
	@echo "Config    : $(CONFIG_H) (CONFIG_SMP=$(CONFIG_SMP), CONFIG_DEBUG_SERIAL=$(CONFIG_DEBUG_SERIAL), MAX_CPUS=$(if $(filter 1,$(CONFIG_SMP)),8,1))"
	@echo "QEMU smp  : $(QEMU_SMP)"
	@echo "Objects   : $(OBJS)"

# ---- 交叉编译器：构建 x86_64-sukios 工具链（Binutils + GCC） ----
# 转发到 cross/Makefile.gcc，源码下载走 gh.dl.ifiss.eu.org 代理，
# 但 Makefile 内仍保持原始 github.com 地址。
gcc:
	$(MAKE) -f cross/Makefile.gcc all

gcc-only:
	$(MAKE) -f cross/Makefile.gcc gcc

cross-clean:
	$(MAKE) -f cross/Makefile.gcc clean

# =============================================================================
# Rust 开发工具链（SukiOS 专属）
# -----------------------------------------------------------------------------
# `make make-rust-env` 给仓库其余用户一键创建 Rust 开发环境：
#   1) 自动探查是否已安装 rust（rustup/rustc/cargo），未安装则经 rustup 安装
#      nightly + rust-src（后续 Servo 移植需要 build-std 编译 std）；
#   2) 构建 SukiOS 用户态运行时静态库 libsuki.a（供 Rust 链接，获得
#      malloc/free/pthread/syscall 等能力，无需从零移植 std）；
#   3) 生成 rust/x86_64-sukios.json 目标规格（含本仓库绝对路径 + 交叉链接器）
#      与 rust/.cargo/config.toml（build.target 指向该规格）；
#   4) 仓库已提交 rust/ 下的示例工程（rust-toolchain.toml / Cargo.toml /
#      src/main.rs），用户 cd rust && cargo build 即可产出可在 SukiOS 运行的 ELF。
# 该环境“属于 SukiOS”：用我们的交叉链接器 + user/user.ld + libsuki.a。
# 仅生成目标规格/归档等产物，不改动内核/现有用户态程序。
# =============================================================================

# Rust 运行时归档：复用 USER_LIB_OBJS，剔除 crt0（Rust 自带 _start）。
RUST_LIB_OBJS := $(filter-out $(BUILD)/user/lib/crt0.S.o,$(USER_LIB_OBJS))
RUST_LIB      := $(BUILD)/libsuki.a
RUST_DIR      := $(CURDIR)/rust

# 归档工具：优先交叉 ar，回退宿主 ar。
ifeq ($(wildcard $(CROSS_OPT)/bin/x86_64-sukios-elf-ar),$(CROSS_OPT)/bin/x86_64-sukios-elf-ar)
  RUST_AR := x86_64-sukios-elf-ar
else
  RUST_AR := ar
endif

# 交叉链接器探测（与 USER_CC 同逻辑，供目标规格写入正确 linker）。
ifeq ($(wildcard $(SUKIOS_CC)),$(SUKIOS_CC))
  RUST_USER_CC := x86_64-sukios-elf-gcc
else ifeq ($(wildcard $(ELF_CC)),$(ELF_CC))
  RUST_USER_CC := x86_64-elf-gcc
else
  RUST_USER_CC := gcc
endif

# 确保用户态配置头先就位（USER_LIB_OBJS 编译依赖 -include $(CONFIG_H)）。
$(RUST_LIB_OBJS): | $(CONFIG_H)

$(RUST_LIB): $(RUST_LIB_OBJS) | $(CONFIG_H)
	@mkdir -p $(dir $@)
	$(RUST_AR) rcs $@ $(RUST_LIB_OBJS)
	@echo "==> SukiOS Rust runtime archive $@ ($(words $(RUST_LIB_OBJS)) objects)"

# 仅重建运行时归档。
rust-libs: $(RUST_LIB)
	@true

# 一键创建 Rust 开发环境（探测/安装 rust + 生成规格 + 建归档）。
make-rust-env: $(RUST_LIB)
	@sh $(CURDIR)/tools/suki-rust-env.sh

# 别名。
rust-env: make-rust-env
	@true

# 构建 rust/ 下的示例工程（cargo 需在 PATH 中；make make-rust-env 已装 rustup）。
rust-build:
	@export PATH="$$HOME/.cargo/bin:$$PATH"; cd $(RUST_DIR) && cargo build

# ---- 用户程序编译规则（必须先于内核通配规则） ----
$(BUILD)/user/%.S.o: user/%.S
	@mkdir -p $(dir $@)
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

# L5：用户态栈金丝雀提供文件必须以 -fno-stack-protector 编译（理由同内核）。
$(BUILD)/user/lib/stack_canary.c.o: user/lib/stack_canary.c
	@mkdir -p $(dir $@)
	$(USER_CC) $(USER_CFLAGS) -fno-stack-protector -c $< -o $@

# FatFs 核心用 freestanding 编译，并包含 drivers/FatFs 头路径（ff.h/ffconf.h）。
# 注意 -Wno-unused-parameter/-Wno-implicit-fallthrough 屏蔽 FatFs 自身体积较大
# 的告警（不影响正确性）；仍保留 -Wall -Wextra 其余项做防御性检查。
$(BUILD)/fatfs/%.c.o: drivers/FatFs/%.c
	@mkdir -p $(dir $@)
	$(USER_CC) $(USER_CFLAGS) -I drivers/FatFs -Wno-unused-parameter \
		-Wno-implicit-fallthrough -c $< -o $@

# fs_server 需要 FatFs 头路径（ff.h/ffconf.h 在 drivers/FatFs/）。
$(BUILD)/user/fs_server.c.o: user/fs_server.c
	@mkdir -p $(dir $@)
	$(USER_CC) $(USER_CFLAGS) -I drivers/FatFs -c $< -o $@

$(BUILD)/user/%.c.o: user/%.c
	@mkdir -p $(dir $@)
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

# 链接为独立 ELF（固定基址 0x400000，由 user/user.ld 决定；PIE 亦可）
# fs_server 额外链接 FatFs 核心（ff.o + ffunicode.o）。
$(BUILD)/user/%.elf: $(BUILD)/user/%.c.o $(USER_LIB_OBJS) user/user.ld
	$(USER_CC) -nostdlib -static -no-pie -Wl,--build-id=none \
		-Wl,--no-warn-rwx-segments -T user/user.ld \
		-o $@ $< $(USER_LIB_OBJS) -lgcc
	@echo "==> user program $@ ($$(stat -c%s $@) bytes)"

# fs_server 专用：追加 FatFs 核心对象
$(BUILD)/user/fs_server.elf: $(BUILD)/user/fs_server.c.o $(USER_LIB_OBJS) $(FATFS_OBJS) user/user.ld
	$(USER_CC) -nostdlib -static -no-pie -Wl,--build-id=none \
		-Wl,--no-warn-rwx-segments -T user/user.ld \
		-o $@ $(BUILD)/user/fs_server.c.o $(USER_LIB_OBJS) $(FATFS_OBJS) -lgcc
	@echo "==> user program $@ ($$(stat -c%s $@) bytes)"

# ---- lwIP 编译/链接（net_server） ----
# lwIP 源文件：独立规则，带 lwIP 头路径（arch/cc.h、lwipopts.h 位于 user/lib）。
$(BUILD)/lwip/%.c.o: $(LWIP_DIR)/%.c
	@mkdir -p $(dir $@)
	$(USER_CC) $(USER_CFLAGS) $(LWIP_INC) -c $< -o $@

# net_server 需要 lwIP 头路径（arch/cc.h / lwipopts.h）。
$(BUILD)/user/net_server.c.o: user/net_server.c
	@mkdir -p $(dir $@)
	$(USER_CC) $(USER_CFLAGS) $(LWIP_INC) -c $< -o $@

# net_server 专用：链接 lwIP 对象 + 用户库。
# --allow-multiple-definition：lwIP 与用户库个别符号（如 htons/memset 内建）可能
# 重复，容忍之，与 FreeType 库的处理方式一致。
$(BUILD)/user/net_server.elf: $(BUILD)/user/net_server.c.o $(USER_LIB_OBJS) $(LWIP_OBJS) user/user.ld
	$(USER_CC) -nostdlib -static -no-pie -Wl,--build-id=none \
		-Wl,--allow-multiple-definition -Wl,--no-warn-rwx-segments -T user/user.ld \
		-o $@ $(BUILD)/user/net_server.c.o $(USER_LIB_OBJS) $(LWIP_OBJS) -lgcc
	@echo "==> user program $@ ($$(stat -c%s $@) bytes)"

# nettest（网络端到端验证）源码位于 user/apps/，单独给出 .c.o 规则；其 .elf 走
# 通用 $(BUILD)/user/%.elf 规则（链接 USER_LIB_OBJS，含 suki_native 的 suki_socket_*）。
$(BUILD)/user/nettest.c.o: user/apps/nettest.c
	@mkdir -p $(dir $@)
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

# 把 ELF 文件作为原始字节流嵌入内核镜像（objcopy -I binary 生成
# _binary_build_user_<name>_elf_start/end 符号），并重命名为
# user_<name>_start / user_<name>_end。内核 ELF 加载器在启动任务时读取
# 这段字节并解析 ELF64（废除平坦二进制）。
#
# 模式 %.ssvc.blob.o 对应逻辑名 fs_server（% 为 fs_server），其依赖为
# $(BUILD)/user/fs_server.elf（由通用 %.elf 规则生成）。符号用 $(basename $*)
# 剥离 .ssvc 后缀，得到 user_fs_server_start / user_fs_server_end，与 kmain.c
# 中硬编码的引用保持一致。
$(BUILD)/user/%.ssvc.blob.o: $(BUILD)/user/%.elf
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
		--redefine-sym _binary_build_user_$*_elf_start=user_$(basename $*)_start \
		--redefine-sym _binary_build_user_$*_elf_end=user_$(basename $*)_end \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		$< $@
	# objcopy -I binary 会丢掉 .note.GNU-stack，导致最终内核 ELF 的
	# GNU_STACK 缺省为「可执行栈」(安全弱点)。补一个空(=non-exec)的
	# .note.GNU-stack 段，确保内核栈不可执行。
	objcopy --add-section .note.GNU-stack=/dev/null $@ $@.nostack && mv $@.nostack $@

# Rust 示例 ELF -> 内核内嵌 blob（符号 _binary_rusthello_start/_end，kmain 经 weak 引用）。
# 仅当 cargo build 产物存在时构建；否则 RUST_BLOB 为空，内核跳过该自检。
ifneq ($(RUST_BIN),)
$(RUST_BLOB): $(RUST_BIN)
	@mkdir -p $(dir $@)
	cp $(RUST_BIN) $(BUILD)/user/rusthello.bin
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
		--redefine-sym _binary_build_user_rusthello_bin_start=_binary_rusthello_start \
		--redefine-sym _binary_build_user_rusthello_bin_end=_binary_rusthello_end \
		--redefine-sym _binary_build_user_rusthello_bin_size=_binary_rusthello_size \
		$(BUILD)/user/rusthello.bin $@
	objcopy --add-section .note.GNU-stack=/dev/null $@ $@.nostack && mv $@.nostack $@
endif

# dltest 是独立程序（user/apps/dltest.c → build/apps/dltest.elf），但内核内嵌
# blob 规则（USER_PROGS）按 user/<name>.elf 命名。这里把 app 产物拷成
# build/user/dltest.elf，使其复用上面的 %.ssvc.blob.o 规则生成内核可见的
# user_dltest_start / user_dltest_end 符号供 kmain 自启动。
$(BUILD)/user/dltest.elf: $(BUILD)/apps/dltest.elf
	@mkdir -p $(dir $@)
	cp $< $@

# ---- 独立程序编译规则（user/apps/*.c，启用 SSE，链接为独立 ELF） ----
$(BUILD)/apps/%.o: user/apps/%.c
	@mkdir -p $(dir $@)
	$(USER_CC) $(APP_CFLAGS) -c $< -o $@

$(BUILD)/apps/%.elf: $(BUILD)/apps/%.o $(USER_LIB_OBJS) user/user.ld
	$(USER_CC) -nostdlib -static -no-pie -Wl,--build-id=none \
		-Wl,--gc-sections -Wl,--no-warn-rwx-segments -T user/user.ld \
		-o $@ $< $(USER_LIB_OBJS) -lgcc
	@echo "==> standalone app $@ ($$(stat -c%s $@) bytes)"

# dltest：动态链接验证程序。
# 编译期经 -Bdynamic -l:libtest.sl 记录 DT_NEEDED=libtest.sl，使链接器对主程序
# 引用到的 libtest.sl 全局变量（sl_const）生成 R_X86_64_COPY 拷贝重定位
# （验证：加载期把库初始值 42 拷入主程序 .bss 副本）。运行期再经
# dlopen("/LIB/libtest2.sl") + dlsym + dlclose 验证「物理页回收」与「槽位复用」。
# --no-dynamic-linker：本系统无用户态动态链接器，由内核 elf_link_dynamic 在
# execve 时完成加载/重定位，故必须去掉 PT_INTERP（否则 elf_validate 拒绝 ET_EXEC）。
$(BUILD)/apps/dltest.elf: $(BUILD)/apps/dltest.o $(USER_LIB_OBJS) user/user.ld $(LIBTEST_SL)
	$(USER_CC) -nostdlib -no-pie -fno-pic -Wl,--build-id=none \
		-Wl,--no-warn-rwx-segments -Wl,--no-dynamic-linker \
		-Wl,-Bdynamic -L$(dir $(LIBTEST_SL)) -l:libtest.sl \
		-T user/user.ld -o $@ $< $(USER_LIB_OBJS) -lgcc
	@echo "==> dynamic-link test $@ ($$(stat -c%s $@) bytes)"

# 字体程序（fontsrv/pchfnt）额外链接 FreeType 静态库。
$(BUILD)/apps/fontsrv.elf: $(BUILD)/apps/fontsrv.o $(USER_LIB_OBJS) $(FT_LIB) user/user.ld
	$(USER_CC) -nostdlib -static -no-pie -Wl,--build-id=none \
		-Wl,--gc-sections -Wl,--no-warn-rwx-segments -T user/user.ld \
		-o $@ $< $(USER_LIB_OBJS) $(FT_LIB) -lgcc
	@echo "==> font service $@ ($$(stat -c%s $@) bytes)"

$(BUILD)/apps/pchfnt.elf: $(BUILD)/apps/pchfnt.o $(USER_LIB_OBJS) $(FT_LIB) user/user.ld
	$(USER_CC) -nostdlib -static -no-pie -Wl,--build-id=none \
		-Wl,--gc-sections -Wl,--no-warn-rwx-segments -T user/user.ld \
		-o $@ $< $(USER_LIB_OBJS) $(FT_LIB) -lgcc
	@echo "==> font tool $@ ($$(stat -c%s $@) bytes)"

# ---- 编译期配置头（SMP 开关的注入载体）----
# 每次 make 都重新求值（FORCE），但只有【内容真正变化】时才改写文件时间戳
# （cmp 比对后丢弃临时文件），避免无误的全量重编。
$(CONFIG_H): FORCE
	@mkdir -p $(BUILD)
	@printf '/* AUTO-GENERATED by Makefile -- do not edit. */\n'  > $@.tmp
	@printf '#ifndef _SUKI_BUILD_CONFIG_H\n'                     >> $@.tmp
	@printf '#define _SUKI_BUILD_CONFIG_H\n'                     >> $@.tmp
	@printf '#define CONFIG_SMP %s\n' '$(CONFIG_SMP)'            >> $@.tmp
	@printf '#define CONFIG_DEBUG_SERIAL %s\n' '$(CONFIG_DEBUG_SERIAL)' >> $@.tmp
	@printf '#define ATA_DEBUG %s\n' '$(CONFIG_DEBUG_SERIAL)'    >> $@.tmp
	@printf '#endif /* _SUKI_BUILD_CONFIG_H */\n'                >> $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi

FORCE:

# ---- 内核编译规则 ----
# `| $(CONFIG_H)` 为 order-only 依赖：保证首次构建与配置变更时配置头先就位。
$(BUILD)/%.S.o: %.S | $(CONFIG_H)
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

# L5：栈金丝雀提供文件（__stack_chk_guard / __stack_chk_fail）必须以
# -fno-stack-protector 编译——否则 __stack_chk_fail 自身被插桩后递归调用
# 自己，且 guard 未初始化前插桩函数会误报栈破坏。故此处覆盖全局 CFLAGS。
$(BUILD)/kernel/stack_canary.c.o: kernel/stack_canary.c | $(CONFIG_H)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-stack-protector -c $< -o $@

$(BUILD)/%.c.o: %.c | $(CONFIG_H)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- 链接内核 ELF（P0-8 KASLR：两阶段，先产重定位表再重链） ----
# 阶段 A：预链接（带 --emit-relocs 保留重定位信息）到 kernel.pre.elf
# 此时 relk.o 尚未生成，g_relk_offsets/g_relk_count 尚未定义，故对预链接
# 允许未解析符号（仅用于抽取重定位表；最终链接仍会严格检查未定义符号）。
PRE := $(BUILD)/kernel.pre.elf
$(PRE): $(OBJS) boot/linker.ld
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -Wl,--emit-relocs -Wl,--unresolved-symbols=ignore-all -o $@ $(OBJS) -lgcc
	@echo "==> Pre-linked $(PRE)"

# 阶段 B：由预链接 ELF 抽取 R_X86_64_64 重定位偏移，生成 build/relk.c
build/relk.c: $(PRE)
	python3 tools/gen_relk.py $< $@

# 阶段 C：编译重定位表为 relk.o
RELK := $(BUILD)/relk.o
$(RELK): build/relk.c | $(CONFIG_H)
	$(CC) $(CFLAGS) -c $< -o $@

# 阶段 D：最终链接（含 relk.o；.text/.data 布局与预链接一致，故偏移有效）
$(KERNEL): $(OBJS) $(RELK) boot/linker.ld
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(RELK) -lgcc
	@echo "==> Linked $(KERNEL)"
	@grub-file --is-x86-multiboot2 $(KERNEL) \
		&& echo "==> valid Multiboot2 kernel" || echo "!! Multiboot2 header INVALID"
	@readelf -SW $(KERNEL) | awk '$$3==".boot"{ \
	    a=strtonum("0x"$$5); o=strtonum("0x"$$6); s=strtonum("0x"$$7); \
	    if (a != 0x100000) { print "!! L4 FAIL: .boot VMA != 1MB (got "a")"; exit 1; } \
	    if (o >= 0x8000 || o+s > 0x8000) { print "!! L4 FAIL: MB2 header beyond first 32KiB (off="o" size="s")"; exit 1; } \
	    print "==> L4 check: .boot VMA=0x"a" fileoff=0x"o" size=0x"s" (within first 32KiB)"; }'

# ---- 生成可引导 ISO (BIOS + UEFI 双启动) ----
iso: $(ISO)
$(ISO): $(KERNEL) grub/grub.cfg configs/display.cfg
	@mkdir -p $(ISODIR)/boot/grub
	cp $(KERNEL) $(ISODIR)/boot/kernel.ski
	cp grub/grub.cfg $(ISODIR)/boot/grub/grub.cfg
	# 显示服务配置文件：经 GRUB module2 加载，内核在 fb_init 前解析。
	# 去掉 `-` 前缀：配置缺失应显式失败而非静默跳过，避免 ISO 不含配置导致
	# 内核误用旧缓存/残留内容而把 video_mode 解析成错误值。
	cp configs/display.cfg $(ISODIR)/boot/display.cfg
	grub-mkrescue -o $(ISO) $(ISODIR) 2>/dev/null
	@echo "==> Built $(ISO)"

# ---- FAT32 磁盘镜像（演示文本 + 独立程序 + MP3 音乐） ----
# 独立程序放入 ::BIN/，文件名一律大写且【无 .elf 后缀】（如 ::BIN/PLAYAUDIO）。
# PLAYAUDIO 超过 8.3 短名 → mtools 自动创建长文件名(LFN)，FS_SERVER 已支持
# 读取 LFN，故 shell 可用 `exec BIN/playaudio` 装载。
disk: $(DISK)
$(DISK): $(APP_ELFS) $(FONT_ELFS) $(LIBTEST_SL) others_tests/moonhalo.mp3 $(RUST_BIN)
	@mkdir -p $(BUILD)
	truncate -s 64M $@
	mformat -i $@ -F -v SUKIOS ::
	printf 'Welcome to SukiOS!\nThis file lives on a FAT32 disk,\nread by the Ring3 FS_SERVER via Mach IPC.\n' > $(BUILD)/README.TXT
	printf 'hello from FAT32 :)\n' > $(BUILD)/HELLO.TXT
	printf 'SukiOS hybrid kernel roadmap:\n- Mach ports\n- FAT32 (RO)\n- Intel HDA audio\n- GUI compositor (soon)\n' > $(BUILD)/ROADMAP.TXT
	mcopy -i $@ $(BUILD)/README.TXT ::README.TXT
	mcopy -i $@ $(BUILD)/HELLO.TXT ::HELLO.TXT
	mcopy -i $@ $(BUILD)/ROADMAP.TXT ::ROADMAP.TXT
	mmd -i $@ ::SYS
	mmd -i $@ ::BIN
	# 图片资源目录：把 images/ 下【全部】.bmp 放入 ::IMAGES/，供 BMP 加载器
	# 诊断显示用。注意 .gitignore 排除了 CG*.bmp（避免误提交大二进制资源），
	# 但生成硬盘镜像时仍从本地 working dir 复制全部 bmp（含被忽略的 CG*.bmp），
	# 故 glob images/*.bmp 会一并带上这些本地存在的图片。
	mmd -i $@ ::IMAGES
	mcopy -i $@ images/*.bmp ::IMAGES/
	# 独立程序（用户态二进制应用）按官方后缀体系使用 .ska：
	# 磁盘文件名 BIN/<NAME>.SKA，shell 的 exec 在找不到原路径时会自动补 .ska，
	# 故 `exec BIN/playaudio` 与 `exec BIN/playaudio.ska` 均可装载。
	@for p in $(APP_PROGS); do \
		up=$$(echo $$p | tr a-z A-Z); \
		echo "  disk: BIN/$$up.SKA  <= $(BUILD)/apps/$$p.elf"; \
		mcopy -i $@ $(BUILD)/apps/$$p.elf ::BIN/$$up.SKA; \
	done
	# 共享库目录：动态链接 / dlopen 运行时加载
	mmd -i $@ ::LIB 2>/dev/null || true
	mcopy -i $@ $(LIBTEST_SL) ::LIB/libtest.sl
	# 同一份库以不同文件名再装一份：供 dltest 运行期 dlopen("/LIB/libtest2.sl")
	# 触发「纯 dlopen（非加载期依赖）」路径，从而验证 dlclose 物理页回收与槽位复用。
	mcopy -i $@ $(LIBTEST_SL) ::LIB/libtest2.sl
	mcopy -i $@ others_tests/moonhalo.mp3 ::MOONHALO.MP3
	# 字体文件目录：把 resources/ 下 .ttf 放入 ::FONTS/，供字体服务内存加载
	mmd -i $@ ::FONTS 2>/dev/null || true
	@for f in resources/*.ttf; do \
		[ -e "$$f" ] || continue; \
		bn=$$(basename $$f | tr a-z A-Z); \
		echo "  disk: FONTS/$$bn <= $$f"; \
		mcopy -i $@ $$f ::FONTS/$$bn; \
	done
	# 字体服务与渲染工具：放入 ::BIN/
	@for p in $(FONT_PROGS); do \
		up=$$(echo $$p | tr a-z A-Z); \
		echo "  disk: BIN/$$up.SKA  <= $(BUILD)/apps/$$p.elf"; \
		mcopy -i $@ $(BUILD)/apps/$$p.elf ::BIN/$$up.SKA; \
	done
	# Rust 示例程序（make make-rust-env + cargo build 产物，见 results/step69.md）。
	# 仅当该 SukiOS ELF 已存在时放入镜像；缺失则跳过（不影响其余程序）。
	@if [ -f $(RUST_DIR)/target/x86_64-sukios/debug/sukios-hello ]; then \
		echo "  disk: BIN/RUSTHELLO.SKA <= $(RUST_DIR)/target/x86_64-sukios/debug/sukios-hello"; \
		mcopy -i $@ $(RUST_DIR)/target/x86_64-sukios/debug/sukios-hello ::BIN/RUSTHELLO.SKA; \
	else \
		echo "  disk: skip RUSTHELLO.SKA (rust binary not built; run make make-rust-env + make rust-build)"; \
	fi
	@echo "==> Built FAT32 disk $(DISK)"

# ---- 运行 (带图形窗口，需 X/GTK) ----
# -boot d 强制从光驱(ISO)引导：磁盘位于 index=0(第一硬盘)无引导扇区，
# 若不指定 -boot d，-machine pc 会优先尝试硬盘导致无法启动。
# 这样 `make run` 即可直接拉起一个完整可启动的 SukiOS 模拟环境。
run: $(ISO) $(DISK)
	$(QEMU_RUN) $(QEMU_FLAGS) $(QEMU_SERIAL) $(QEMU_AUDIO) $(QEMU_NET) -boot d -cdrom $(ISO) $(QEMU_DISK)

# ---- 无头运行 (仅串口，用于自动化验证) ----
run-headless: $(ISO) $(DISK)
	$(QEMU_RUN) $(QEMU_FLAGS) -display none $(QEMU_SERIAL) $(QEMU_AUDIO) $(QEMU_NET) -boot d -cdrom $(ISO) $(QEMU_DISK)

# ---- 调试运行（带全部串口冗长诊断） ----
# 通过递归子 make 把 DBG=1 作为全局变量传入，确保 build/config.h 生成
# CONFIG_DEBUG_SERIAL=1（触发全量重编），从而输出 IPC/disk-srv/console-srv
# 的逐条追踪日志。其余与 run 完全一致。-display none 便于无图形环境自动化
# 验证（串口即诊断输出通道）。
run-dbg:
	$(MAKE) DBG=1 run-headless

# P0-7：磁盘挂 AHCI（DMA+中断），验证 kernel/drivers/ahci.c
run-ahci: $(ISO) $(DISK)
	$(QEMU_RUN) $(QEMU_FLAGS) $(QEMU_SERIAL) $(QEMU_AUDIO) $(QEMU_NET) -boot d -cdrom $(ISO) $(QEMU_AHCI_DISK)

run-ahci-headless: $(ISO) $(DISK)
	$(QEMU_RUN) $(QEMU_FLAGS) -display none $(QEMU_SERIAL) $(QEMU_AUDIO) $(QEMU_NET) -boot d -cdrom $(ISO) $(QEMU_AHCI_DISK)

# ---- P0-6 UEFI：OVMF 启动（GRUB-EFI -> multiboot2 -> 同一 kernel.elf） ----
# VARS 每次从模板复制（保持只读模板干净；EFI 变量写入进副本）。
# 注意 UEFI 显示走 GOP：GRUB 依 MB2 tag5 请求 1024x768x32，OVMF GOP 提供
# 线性帧缓冲经 tag8 转交内核，与 BIOS VBE 路径在内核侧完全同构。
$(OVMF_VARS): $(OVMF_VARS_SRC)
	@mkdir -p $(BUILD)
	cp $< $@

run-uefi: $(ISO) $(DISK) $(OVMF_VARS)
	$(QEMU_RUN) $(QEMU_FLAGS) $(QEMU_UEFI) $(QEMU_SERIAL) $(QEMU_AUDIO) $(QEMU_NET) -boot d -cdrom $(ISO) $(QEMU_AHCI_DISK)

run-uefi-headless: $(ISO) $(DISK) $(OVMF_VARS)
	$(QEMU_RUN) $(QEMU_FLAGS) $(QEMU_UEFI) -display none $(QEMU_SERIAL) $(QEMU_AUDIO) $(QEMU_NET) -boot d -cdrom $(ISO) $(QEMU_AHCI_DISK)

# ---- GDB 调试 (配合 .gdbinit) ----
debug: $(ISO) $(DISK)
	$(QEMU) $(QEMU_FLAGS) -display none $(QEMU_SERIAL) $(QEMU_AUDIO) $(QEMU_NET) -boot d -cdrom $(ISO) $(QEMU_DISK) -s -S

# ---- PVH 直启 (GRUB 不可用时)：qemu -kernel 走 PVH 协议加载同一 kernel.elf ----
# 不经 ISO/GRUB；-kernel 扫描 ELF SHT_NOTE 段的 Xen PVH note 取得入口。
# -append "pvh" 仅作标识（PVH 下 EAX!=MULTIBOOT2_MAGIC，内核据此分派）。
run-q: $(KERNEL) $(DISK)
	$(QEMU_RUN) $(QEMU_FLAGS) -display none $(QEMU_SERIAL) $(QEMU_AUDIO) \
		-kernel $(KERNEL) -append "pvh" $(QEMU_DISK)

# PVH 直启 + 异常日志（qemu -d int 捕获 #GP/#PF 精确 RIP/error code 到 qemu.int.log）
run-q-debug: $(KERNEL) $(DISK)
	$(QEMU_RUN) $(QEMU_FLAGS) -display none $(QEMU_SERIAL) $(QEMU_AUDIO) \
		-kernel $(KERNEL) -append "pvh" $(QEMU_DISK) \
		-d int -D qemu.int.log -no-reboot

clean:
	rm -rf $(BUILD)

# ---- 头文件依赖自动包含（由 -MMD 生成的 .d 文件） ----
-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
