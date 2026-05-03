# =============================================================================
# SukiOS 构建脚本 (支持用户程序自动编译)
# =============================================================================

CC      = gcc
LD      = gcc
NASM    = nasm

CFLAGS  = -ffreestanding -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
          -mcmodel=large -fno-pic -fno-pie -O2 -Wall -Wextra \
          -Wno-unused-parameter -std=gnu11 -I include

LDFLAGS = -ffreestanding -nostdlib -static -z max-page-size=0x1000 \
          -T boot/linker.ld

NASMFLAGS = -f elf64

# ---------- 用户程序编译选项 ----------
USER_CC       = x86_64-linux-gnu-gcc         # 如无此工具，可改为 x86_64-linux-gnu-gcc
USER_CFLAGS   = -ffreestanding -nostdlib -static -O2
USER_LDFLAGS  = -Wl,-T app/user.ld
USER_ELF_DIR  = bin

# ---------- 源文件 ----------
BOOT_ASM = boot/boot.asm

KERNEL_C_SRCS   = $(shell find kernel -name '*.c')
KERNEL_ASM_SRCS = $(shell find kernel -name '*.asm')

APP_DIRS    = $(shell find app -mindepth 1 -maxdepth 1 -type d 2>/dev/null)
APP_NAMES   = $(notdir $(APP_DIRS))
APP_ELFS    = $(addprefix $(USER_ELF_DIR)/, $(APP_NAMES))

# ---------- 构建输出 ----------
BUILD_DIR      = build
BOOT_OBJ       = $(BUILD_DIR)/boot.o

KERNEL_C_OBJS  = $(patsubst kernel/%.c,$(BUILD_DIR)/%.o,$(KERNEL_C_SRCS))
KERNEL_ASM_OBJS = $(patsubst kernel/%.asm,$(BUILD_DIR)/%.o,$(KERNEL_ASM_SRCS))
KERNEL_OBJS    = $(KERNEL_C_OBJS) $(KERNEL_ASM_OBJS)

KERNEL_ELF     = $(BUILD_DIR)/kernel.elf
ISO_DIR        = $(BUILD_DIR)/iso
ISO            = $(BUILD_DIR)/sukios.iso

LINKER   = boot/linker.ld
GRUB_CFG = boot/grub.cfg

.PHONY: all apps verify iso run run-direct debug clean dirs disk disk-update

all: dirs $(KERNEL_ELF) apps

dirs:
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(USER_ELF_DIR)

$(BOOT_OBJ): $(BOOT_ASM)
	$(NASM) $(NASMFLAGS) $< -o $@

$(BUILD_DIR)/%.o: kernel/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: kernel/%.asm
	@mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) $< -o $@

$(KERNEL_ELF): $(BOOT_OBJ) $(KERNEL_OBJS) $(LINKER)
	$(LD) $(LDFLAGS) -o $@ $(BOOT_OBJ) $(KERNEL_OBJS)

# ========== 用户程序（自动扫描） ==========
ifneq ($(APP_DIRS),)
apps: $(APP_ELFS)

$(APP_ELFS): $(USER_ELF_DIR)/%: app/%/main.c
	@echo "[APPS] Compiling user program '$*'..."
	$(USER_CC) $(USER_CFLAGS) -o $@ $< $(USER_LDFLAGS)
else
apps:
	@echo "[APPS] No user applications found (app/ directory is empty)."
endif

# ========== 磁盘镜像 ==========
disk:
	@echo "[DISK] Creating 64M disk image..."
	qemu-img create disk.img 64M
	mkfs.vfat -F 32 disk.img

disk-update: apps disk.img
	@if [ -n "$(APP_ELFS)" ]; then \
		for elf in $(APP_ELFS); do \
			mcopy -o -i disk.img $$elf ::/ ; \
		done; \
		echo "[DISK] User apps updated."; \
	else \
		echo "[DISK] No user apps to copy."; \
	fi

# ========== 启动 ==========
verify: $(KERNEL_ELF)
	@grub-file --is-x86-multiboot2 $(KERNEL_ELF) && \
		echo "Multiboot2 header OK" || \
		echo "Multiboot2 header FAILED"

iso: $(KERNEL_ELF)
	@mkdir -p $(ISO_DIR)/boot/grub
	@cp $(KERNEL_ELF) $(ISO_DIR)/boot/kernel.elf
	@cp $(GRUB_CFG) $(ISO_DIR)/boot/grub/grub.cfg
	@grub-mkrescue -o $(ISO) $(ISO_DIR) 2>/dev/null
	@echo "[ISO] $(ISO) created"

run: all disk apps disk-update iso
	@echo "[QEMU] Starting SukiOS with disk..."
	qemu-system-x86_64 -cdrom $(ISO) -m 128M -serial file:serial.log \
		-drive file=disk.img,format=raw,if=ide -boot d -monitor stdio

run-direct: $(KERNEL_ELF)
	qemu-system-x86_64 -kernel $(KERNEL_ELF) -m 128M -serial file:serial.log

debug: iso
	qemu-system-x86_64 -cdrom $(ISO) -m 128M -serial stdio -s -S &
	@echo "GDB server on port 1234"
	@echo "Connect: gdb $(KERNEL_ELF) -ex 'target remote :1234'"

clean:
	rm -rf $(BUILD_DIR)
	rm -rf $(USER_ELF_DIR)
	rm -f serial.log disk.img