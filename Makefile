# =============================================================================
# SukiOS 顶层 Makefile
# -----------------------------------------------------------------------------
# 工具链自动探测：若系统已安装 x86_64-elf-gcc 交叉编译器则优先使用，
# 否则回退到原生 gcc（配合 -ffreestanding -nostdlib，x86_64 主机可正常产出
# 自由环境内核）。安装交叉工具链后无需改动本文件即可无缝切换。
# =============================================================================

ARCH := x86_64

# ---- 工具链探测 ----
ifneq (,$(shell command -v x86_64-elf-gcc 2>/dev/null))
  CC := x86_64-elf-gcc
  TOOLCHAIN := cross (x86_64-elf)
else
  CC := gcc
  TOOLCHAIN := native (host gcc, freestanding)
endif

# ---- 目录 ----
BUILD  := build
ISODIR := $(BUILD)/isodir
KERNEL := $(BUILD)/kernel.elf
ISO    := $(BUILD)/SukiOS.iso

# ---- 编译/链接选项 ----
# 说明：手册 CFLAGS 原写 -mcmodel=kernel，但该模型要求内核位于顶部 2GB
# (0xFFFFFFFF80000000+)，与红线 KERNEL_BASE=0xFFFF800000000000 冲突，
# 故改用 -mcmodel=large（支持任意 64 位地址）。
CFLAGS := -ffreestanding -nostdlib -std=gnu11 -Wall -Wextra -O2 \
          -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mgeneral-regs-only \
          -mcmodel=large -fno-pic -fno-pie -fno-stack-protector \
          -fno-asynchronous-unwind-tables -fno-omit-frame-pointer \
          -I include

ASFLAGS := -ffreestanding -mcmodel=large -fno-pic -fno-pie -I include

LDFLAGS := -nostdlib -static -no-pie -z max-page-size=0x1000 \
           -Wl,--build-id=none -T boot/linker.ld

# ---- 源文件 ----
C_SRCS := $(shell find kernel -name '*.c' 2>/dev/null)
S_SRCS := boot/boot.S boot/multiboot2_header.S $(shell find kernel -name '*.S' 2>/dev/null)

# ---- Ring3 系统服务（编译为 ELF，以字节流嵌入内核镜像，开机由内核直接装载） ----
USER_PROGS   := fs_server input_server shell
USER_CFLAGS  := -ffreestanding -nostdlib -std=gnu11 -Wall -Wextra -O2 \
                -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mgeneral-regs-only \
                -mcmodel=small -fno-pic -fno-pie -fno-stack-protector \
                -fno-asynchronous-unwind-tables -I user -I include
USER_LIB_OBJS := $(BUILD)/user/lib/crt0.S.o $(BUILD)/user/lib/suki.c.o
USER_BLOBS    := $(patsubst %,$(BUILD)/user/%.blob.o,$(USER_PROGS))

# ---- 独立程序（standalone apps，源码在 user/apps/）----
# 这些程序不嵌入内核，仅放入 FAT32 磁盘的 ::BIN/ 目录（文件名无 .elf 后缀），
# 由 shell 经 exec/spawn 从磁盘装载。区别于系统服务：
#   * 可使用浮点 / SSE（minimp3 MP3 解码依赖），故启用 -msse2 且去掉
#     -mgeneral-regs-only（内核 switch.S 已 fxsave/fxrstor 保存 Ring3 SSE 上下文）。
#   * -Os 优先缩小体积，以适配内核 execve 单条 OOL(16 页=64KiB) 的加载上限。
APP_PROGS    := hello playaudio audiotest
APP_CFLAGS   := -ffreestanding -nostdlib -std=gnu11 -Os \
                -mno-red-zone -msse -msse2 \
                -ffunction-sections -fdata-sections \
                -mcmodel=small -fno-pic -fno-pie -fno-stack-protector \
                -fno-asynchronous-unwind-tables -I user -I include \
                -I user/lib/shims -I minimp3
APP_ELFS     := $(patsubst %,$(BUILD)/apps/%.elf,$(APP_PROGS))

OBJS := $(patsubst %,$(BUILD)/%.o,$(C_SRCS) $(S_SRCS)) $(USER_BLOBS)

# ---- 磁盘镜像 (FAT32) ----
DISK := $(BUILD)/disk.img

# ---- QEMU ----
# 注意：-machine pc (i440FX) 提供传统 IDE (PIIX3, PIO 0x1F0)；
#       q35 只有 AHCI，ATA PIO 驱动无法使用。
QEMU        := qemu-system-x86_64
# 注意：-no-reboot 会让 QEMU 把 8042 复位脉冲当作"关机"处理，配合
#       -no-shutdown 会进入 paused 状态而非真正重启。故移除 -no-reboot，
#       使内核的 sys_reboot()（8042 0xFE 脉冲）可触发真正的机器重启。
# KVM 自动检测：宿主机有 /dev/kvm 时启用硬件虚拟化（浮点/整数全部原生
# 执行，minimp3 解码可达实时数十倍；纯 TCG 仿真解码仅 ~1/8 实时，音频
# 必然断续）。可用 make run QEMU_ACCEL=tcg 强制回退软件仿真。
QEMU_ACCEL  ?= $(shell test -w /dev/kvm && echo kvm || echo tcg)
ifeq ($(QEMU_ACCEL),kvm)
QEMU_FLAGS  := -machine pc,accel=kvm -cpu host -m 2G -no-shutdown
else
QEMU_FLAGS  := -machine pc -cpu qemu64 -m 2G -no-shutdown
endif
QEMU_DISK   := -drive file=$(DISK),format=raw,index=0,media=disk
QEMU_SERIAL := -serial stdio

# ---- 音频：Intel HDA 控制器 (8086:2668, ICH6) + 输出编解码器 ----
# 我们的内核驱动通过 PCI class 0x04/subclass 0x03 探测该控制器。
# QEMU_AUDIODRV 可覆盖后端：pa(PulseAudio，WSLg/桌面默认)、alsa、sdl、
# 或 none(无声，仅供无宿主音频环境下验证 DMA 通路不崩)。
# 例：make run QEMU_AUDIODRV=alsa   或   make run-headless QEMU_AUDIODRV=none
QEMU_AUDIODRV ?= pa
QEMU_AUDIO  := -audiodev $(QEMU_AUDIODRV),id=snd0 \
               -device intel-hda -device hda-duplex,audiodev=snd0

.PHONY: all iso run run-headless debug clean info disk

all: $(KERNEL)

info:
	@echo "Toolchain : $(TOOLCHAIN)"
	@echo "CC        : $(CC)"
	@echo "Objects   : $(OBJS)"

# ---- 用户程序编译规则（必须先于内核通配规则） ----
$(BUILD)/user/%.S.o: user/%.S
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/%.c.o: user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

# 链接为独立 ELF（固定基址 0x400000，由 user/user.ld 决定；PIE 亦可）
$(BUILD)/user/%.elf: $(BUILD)/user/%.c.o $(USER_LIB_OBJS) user/user.ld
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none \
		-Wl,--no-warn-rwx-segments -T user/user.ld \
		-o $@ $< $(USER_LIB_OBJS)
	@echo "==> user program $@ ($$(stat -c%s $@) bytes)"

# 把 ELF 文件作为原始字节流嵌入内核镜像（objcopy -I binary 生成
# _binary_build_user_<name>_elf_start/end 符号），并重命名为
# user_<name>_start / user_<name>_end。内核 ELF 加载器在启动任务时读取
# 这段字节并解析 ELF64（废除平坦二进制）。
$(BUILD)/user/%.blob.o: $(BUILD)/user/%.elf
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
		--redefine-sym _binary_build_user_$*_elf_start=user_$*_start \
		--redefine-sym _binary_build_user_$*_elf_end=user_$*_end \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		$< $@

# ---- 独立程序编译规则（user/apps/*.c，启用 SSE，链接为独立 ELF） ----
$(BUILD)/apps/%.o: user/apps/%.c
	@mkdir -p $(dir $@)
	$(CC) $(APP_CFLAGS) -c $< -o $@

$(BUILD)/apps/%.elf: $(BUILD)/apps/%.o $(USER_LIB_OBJS) user/user.ld
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none \
		-Wl,--gc-sections -Wl,--no-warn-rwx-segments -T user/user.ld \
		-o $@ $< $(USER_LIB_OBJS)
	@echo "==> standalone app $@ ($$(stat -c%s $@) bytes)"

# ---- 内核编译规则 ----
$(BUILD)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- 链接内核 ELF ----
$(KERNEL): $(OBJS) boot/linker.ld
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) -lgcc
	@echo "==> Linked $(KERNEL)"
	@grub-file --is-x86-multiboot2 $(KERNEL) \
		&& echo "==> valid Multiboot2 kernel" || echo "!! Multiboot2 header INVALID"

# ---- 生成可引导 ISO (BIOS + UEFI 双启动) ----
iso: $(ISO)
$(ISO): $(KERNEL) grub/grub.cfg
	@mkdir -p $(ISODIR)/boot/grub
	cp $(KERNEL) $(ISODIR)/boot/kernel.elf
	cp grub/grub.cfg $(ISODIR)/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(ISODIR) 2>/dev/null
	@echo "==> Built $(ISO)"

# ---- FAT32 磁盘镜像（演示文本 + 独立程序 + MP3 音乐） ----
# 独立程序放入 ::BIN/，文件名一律大写且【无 .elf 后缀】（如 ::BIN/PLAYAUDIO）。
# PLAYAUDIO 超过 8.3 短名 → mtools 自动创建长文件名(LFN)，FS_SERVER 已支持
# 读取 LFN，故 shell 可用 `exec BIN/playaudio` 装载。
disk: $(DISK)
$(DISK): $(APP_ELFS) others_tests/moonhalo.mp3
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
	@for p in $(APP_PROGS); do \
		up=$$(echo $$p | tr a-z A-Z); \
		echo "  disk: BIN/$$up  <= $(BUILD)/apps/$$p.elf"; \
		mcopy -i $@ $(BUILD)/apps/$$p.elf ::BIN/$$up; \
	done
	mcopy -i $@ others_tests/moonhalo.mp3 ::MOONHALO.MP3
	@echo "==> Built FAT32 disk $(DISK)"

# ---- 运行 (带图形窗口，需 X/GTK) ----
# -boot d 强制从光驱(ISO)引导：磁盘位于 index=0(第一硬盘)无引导扇区，
# 若不指定 -boot d，-machine pc 会优先尝试硬盘导致无法启动。
# 这样 `make run` 即可直接拉起一个完整可启动的 SukiOS 模拟环境。
run: $(ISO) $(DISK)
	$(QEMU) $(QEMU_FLAGS) $(QEMU_SERIAL) $(QEMU_AUDIO) -boot d -cdrom $(ISO) $(QEMU_DISK)

# ---- 无头运行 (仅串口，用于自动化验证) ----
run-headless: $(ISO) $(DISK)
	$(QEMU) $(QEMU_FLAGS) -display none $(QEMU_SERIAL) $(QEMU_AUDIO) -boot d -cdrom $(ISO) $(QEMU_DISK)

# ---- GDB 调试 (配合 .gdbinit) ----
debug: $(ISO) $(DISK)
	$(QEMU) $(QEMU_FLAGS) -display none $(QEMU_SERIAL) $(QEMU_AUDIO) -boot d -cdrom $(ISO) $(QEMU_DISK) -s -S

clean:
	rm -rf $(BUILD)
