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

# ---- Ring3 用户程序（编译为平坦二进制后以 blob 形式嵌入内核镜像） ----
USER_PROGS   := fs_server input_server shell
USER_CFLAGS  := -ffreestanding -nostdlib -std=gnu11 -Wall -Wextra -O2 \
                -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mgeneral-regs-only \
                -mcmodel=small -fno-pic -fno-pie -fno-stack-protector \
                -fno-asynchronous-unwind-tables -I user -I include
USER_LIB_OBJS := $(BUILD)/user/lib/crt0.S.o $(BUILD)/user/lib/suki.c.o
USER_BLOBS    := $(patsubst %,$(BUILD)/user/%.blob.o,$(USER_PROGS))

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
QEMU_FLAGS  := -machine pc -cpu qemu64 -m 2G -no-shutdown
QEMU_DISK   := -drive file=$(DISK),format=raw,index=0,media=disk
QEMU_SERIAL := -serial stdio

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

$(BUILD)/user/%.elf: $(BUILD)/user/%.c.o $(USER_LIB_OBJS) user/user.ld
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none \
		-Wl,--no-warn-rwx-segments -T user/user.ld \
		-o $@ $< $(USER_LIB_OBJS)

$(BUILD)/user/%.bin: $(BUILD)/user/%.elf
	objcopy -O binary $< $@
	@echo "==> user program $@ ($$(stat -c%s $@) bytes)"

# 将平坦二进制包装为可链接对象，并重命名符号为 user_<name>_start/end
$(BUILD)/user/%.blob.o: $(BUILD)/user/%.bin
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
		--redefine-sym _binary_build_user_$*_bin_start=user_$*_start \
		--redefine-sym _binary_build_user_$*_bin_end=user_$*_end \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		$< $@

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

# ---- FAT32 磁盘镜像（含演示文件） ----
disk: $(DISK)
$(DISK):
	@mkdir -p $(BUILD)
	truncate -s 64M $@
	mformat -i $@ -F -v SUKIOS ::
	printf 'Welcome to SukiOS!\nThis file lives on a FAT32 disk,\nread by the Ring3 FS_SERVER via Mach IPC.\n' > $(BUILD)/README.TXT
	printf 'hello from FAT32 :)\n' > $(BUILD)/HELLO.TXT
	printf 'SukiOS hybrid kernel roadmap:\n- Mach ports\n- FAT32 (RO)\n- GUI compositor (soon)\n' > $(BUILD)/ROADMAP.TXT
	mcopy -i $@ $(BUILD)/README.TXT ::README.TXT
	mcopy -i $@ $(BUILD)/HELLO.TXT ::HELLO.TXT
	mcopy -i $@ $(BUILD)/ROADMAP.TXT ::ROADMAP.TXT
	mmd -i $@ ::SYS
	@echo "==> Built FAT32 disk $(DISK)"

# ---- 运行 (带图形窗口，需 X/GTK) ----
# -boot d 强制从光驱(ISO)引导：磁盘位于 index=0(第一硬盘)无引导扇区，
# 若不指定 -boot d，-machine pc 会优先尝试硬盘导致无法启动。
# 这样 `make run` 即可直接拉起一个完整可启动的 SukiOS 模拟环境。
run: $(ISO) $(DISK)
	$(QEMU) $(QEMU_FLAGS) $(QEMU_SERIAL) -boot d -cdrom $(ISO) $(QEMU_DISK)

# ---- 无头运行 (仅串口，用于自动化验证) ----
run-headless: $(ISO) $(DISK)
	$(QEMU) $(QEMU_FLAGS) -display none $(QEMU_SERIAL) -boot d -cdrom $(ISO) $(QEMU_DISK)

# ---- GDB 调试 (配合 .gdbinit) ----
debug: $(ISO) $(DISK)
	$(QEMU) $(QEMU_FLAGS) -display none $(QEMU_SERIAL) -boot d -cdrom $(ISO) $(QEMU_DISK) -s -S

clean:
	rm -rf $(BUILD)
