# .gdbinit —— SukiOS 调试脚本（手册 8.2）
# 用法：终端1 执行 `make debug`（QEMU -s -S 暂停等待），终端2 执行 `gdb`。
set architecture i386:x86-64
target remote localhost:1234
add-symbol-file build/kernel.elf 0xFFFF800000100000
break kmain
continue
