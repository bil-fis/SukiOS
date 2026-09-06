// rust/src/main.rs
//
// SukiOS 上的最小 Rust(no_std) 可执行示例 —— 同时是“Rust 在 SukiOS 运行”的
// 模板（第二部分将把它装入磁盘经 QEMU 验证）。
//
// 构建链路（由 make make-rust-env 配置）：
//   * 交叉链接器 x86_64-sukios-elf-gcc
//   * 链接脚本    user/user.ld（固定基址 0x400000）
//   * 运行时归档  libsuki.a（SukiOS 用户态 libc：malloc/free/pthread/syscall）
// 入口 _start 经标准 x86_64 `syscall` 指令（System V AMD64 ABI）调用 SukiOS 内核。
//
// syscall 约定（与 user/lib/suki.h 内联汇编、include/sukios/posix.h 号表一致）：
//   %rax = 调用号，%rdi,%rsi,%rdx,%r10,%r8,%r9 = 参数。
#![no_std]
#![no_main]

use core::arch::asm;
use core::panic::PanicInfo;

// 最小 syscall 号（来自 include/sukios/posix.h，与 C 侧 suki.h 单一真相源一致）
const SYS_DEBUG_WRITE: u64 = 4; // sys_debug_write(const char *s, size_t len)
const SYS_TASK_EXIT: u64 = 2;   // sys_task_exit(int code)

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

/// 经 x86_64 `syscall` 指令发出调用。rcx/r11 被 syscall 指令破坏，需声明为 clobber。
#[inline(always)]
unsafe fn syscall(num: u64, a1: u64, a2: u64, a3: u64, a4: u64, a5: u64) -> u64 {
    let ret: u64;
    asm!(
        "syscall",
        in("rax") num,
        in("rdi") a1,
        in("rsi") a2,
        in("rdx") a3,
        in("r10") a4,
        in("r8") a5,
        lateout("rax") ret,
        out("rcx") _,
        out("r11") _,
    );
    ret
}

#[no_mangle]
pub extern "C" fn _start() -> ! {
    let msg = b"hello from rust on SukiOS\n";
    unsafe {
        // sys_debug_write(msg_ptr, msg_len)
        syscall(SYS_DEBUG_WRITE, msg.as_ptr() as u64, msg.len() as u64, 0, 0, 0);
        // sys_task_exit(0)
        syscall(SYS_TASK_EXIT, 0, 0, 0, 0, 0);
    }
    loop {}
}
