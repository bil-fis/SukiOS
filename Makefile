# =============================================================================
# SukiOS 构建脚本
#
# 编译环境：WSL (Windows Subsystem for Linux)
# 目标工具链：GCC + NASM
#
# 前置条件（在 WSL 中执行）：
#   sudo apt update
#   sudo apt install nasm qemu-system-x86 grub-pc-bin grub-common xorriso gcc
#
# 使用方法：
#   make          # 编译 kernel.elf
#   make verify   # 验证 Multiboot2 头部
#   make iso      # 生成可启动 ISO
#   make run      # 生成 ISO 并在 QEMU 中运行
#   make clean    # 清理构建产物
# =============================================================================
CC      = gcc
LD      = gcc
NASM    = nasm

# 编译选项
# -mcmodel=large: 内核在高半区 (0xFFFFFFFF80000000)，需要绝对 64 位寻址
CFLAGS  = -ffreestanding -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
          -mcmodel=large -fno-pic -fno-pie -O2 -Wall -Wextra \
          -Wno-unused-parameter -std=gnu11 -I include

LDFLAGS = -ffreestanding -nostdlib -static -z max-page-size=0x1000 \
          -T boot/linker.ld

NASMFLAGS = -f elf64

# 源文件
BOOT_ASM = boot/boot.asm
KERNEL_C = kernel/kernel.c
LINKER   = boot/linker.ld
GRUB_CFG = boot/grub.cfg

# 构建输出
BUILD_DIR  = build
BOOT_OBJ   = $(BUILD_DIR)/boot.o
KERNEL_OBJ = $(BUILD_DIR)/kernel.o
KERNEL_ELF = $(BUILD_DIR)/kernel.elf
ISO_DIR    = $(BUILD_DIR)/iso
ISO        = $(BUILD_DIR)/sukios.iso

.PHONY: all verify iso run run-direct debug clean dirs

# 默认目标：编译内核
all: dirs $(KERNEL_ELF)

# 创建构建目录
dirs:
	@mkdir -p $(BUILD_DIR)

# 汇编引导代码（NASM → ELF64 目标文件）
$(BOOT_OBJ): $(BOOT_ASM)
	$(NASM) $(NASMFLAGS) $< -o $@

# 编译内核 C 代码
$(KERNEL_OBJ): $(KERNEL_C) include/multiboot2.h
	$(CC) $(CFLAGS) -c $< -o $@

# 链接为 ELF64 可执行文件
# -lgcc：链接编译器支持库（64 位算术运算等需要）
$(KERNEL_ELF): $(BOOT_OBJ) $(KERNEL_OBJ) $(LINKER)
	$(LD) $(LDFLAGS) -o $@ $(BOOT_OBJ) $(KERNEL_OBJ)

# 验证 Multiboot2 头部是否合法
verify: $(KERNEL_ELF)
	@grub-file --is-x86-multiboot2 $(KERNEL_ELF) && \
		echo "Multiboot2 头部验证：通过" || \
		echo "Multiboot2 头部验证：失败"

# 生成可启动 ISO（使用 GRUB2 的 grub-mkrescue）
iso: $(KERNEL_ELF)
	@mkdir -p $(ISO_DIR)/boot/grub
	@cp $(KERNEL_ELF) $(ISO_DIR)/boot/kernel.elf
	@cp $(GRUB_CFG) $(ISO_DIR)/boot/grub/grub.cfg
	@grub-mkrescue -o $(ISO) $(ISO_DIR) 2>/dev/null
	@echo "ISO 已生成：$(ISO)"

# 在 QEMU 中运行 ISO
run: iso
	qemu-system-x86_64 -cdrom $(ISO) -m 128M -serial stdio

# 直接通过 QEMU 内核加载运行（无需 ISO）
run-direct: $(KERNEL_ELF)
	qemu-system-x86_64 -kernel $(KERNEL_ELF) -m 128M

# 使用 GDB 调试
debug: iso
	qemu-system-x86_64 -cdrom $(ISO) -m 128M -serial stdio -s -S &
	@echo "QEMU 已启动 GDB 服务器（端口 1234）"
	@echo "连接方式：gdb $(KERNEL_ELF) -ex 'target remote :1234'"

# 清理构建产物
clean:
	rm -rf $(BUILD_DIR)
