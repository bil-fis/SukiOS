// sukios 是自定义目标，std 视其为「受限平台」，需显式开启该 feature 才能使用 std。
#![feature(restricted_std)]
#![no_main]

use std::arch::asm;

/// 绕过 std、直接用 SukiNative SYS_DEBUG_WRITE(=4) 写串口的诊断输出。
/// 约定与 `sukios_ffi::write` 一致：a1=buf, a2=len（fd 被内核忽略）。
#[inline(never)]
unsafe fn raw_write(s: &[u8]) {
    asm!(
        "mov rax, 4",
        "mov rdi, {buf}",
        "mov rsi, {len}",
        "syscall",
        buf = in(reg) s.as_ptr(),
        len = in(reg) s.len(),
        out("rax") _, out("rdi") _, out("rsi") _, out("rdx") _,
        out("rcx") _, out("r11") _,
        options(nostack)
    );
}

#[no_mangle]
pub unsafe extern "C" fn _start() -> ! {
    raw_write(b"S0 _start\n");

    // panic hook 用裸 syscall 写出崩溃位置（std 的 stdout 可能自身异常，故绕过）。
    std::panic::set_hook(Box::new(|info: &std::panic::PanicInfo| {
        unsafe {
            raw_write(b"PANIC@");
            if let Some(l) = info.location() {
                let f = l.file();
                raw_write(f.as_bytes());
            }
            raw_write(b"\n");
        }
    }));
    raw_write(b"S1 hook_set\n");

    let sp: usize;
    asm!("mov {}, rsp", out(reg) sp, options(nostack));
    let _argc = *(sp as *const i32);
    let _argv = sp.wrapping_add(8) as *const *const u8;
    let _ = (_argc, _argv);

    // 防御性栈对齐：进入测试体前把 %rsp 对齐到 16 字节并预留 8 字节返回槽。
    asm!(
        "and rsp, 0xFFFFFFFFFFFFFFF0",
        "sub rsp, 8",
        "call {0}",
        in(reg) std_test_main,
    );
    std::process::exit(0);
}

unsafe extern "C" fn std_test_main() {
    raw_write(b"T0\n");
    println!("[stdtest] hello from rust std on SukiOS");
    raw_write(b"T1\n");

    let _v: Vec<u32> = (0..3).collect();
    raw_write(b"T2\n");
    println!("args = {:?}", std::env::args().collect::<Vec<_>>());
    raw_write(b"T3\n");

    let mut s = String::from("abc");
    s.push('d');
    raw_write(b"T4\n");
    println!("s = {}", s);
    raw_write(b"T5\n");

    let mut m: std::collections::HashMap<u32, u32> = std::collections::HashMap::new();
    m.insert(1, 2);
    raw_write(b"T6\n");
    println!("m = {:?}", m);
    raw_write(b"T7\n");

    let _cur = std::thread::current();
    let _n = _cur.name();
    raw_write(b"T8\n");
    println!("thread name = {:?}", _n);
    raw_write(b"T9\n");

    let t0 = std::time::Instant::now();
    let _x: u64 = (0..1000).map(|i| i as u64).sum();
    let _ = t0.elapsed();
    raw_write(b"T10\n");
    println!("[stdtest] PASS");
    raw_write(b"T11\n");
}
