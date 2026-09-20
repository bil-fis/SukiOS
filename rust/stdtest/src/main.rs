// rust/stdtest/src/main.rs
//
// Rust 标准库（std）在 SukiOS 上的冒烟测试：
//   * println! 走 stdio（std 的 sys::stdio 后端）
//   * Vec/String/HashMap 走 std 的全局分配器（sys::alloc 后端）
//   * std::env::args 走 sys::args 后端
//   * std::time 走 sys::time 后端
// 构建：cd rust/stdtest && cargo +nightly build -Zbuild-std=std,panic_abort
//
// 注意：sukios 是自定义目标，std 视其为「受限平台」，需开启 restricted_std；
// 且 Rust 的 std 二进制默认无 crt0、`_start` 入口，故用 #![no_main] 自行提供
// System V 初始栈入口 `_start`，最后用公开的 std::process::exit 终止。

#![feature(restricted_std)]
#![no_main]

// SukiOS 内核按 System V 初始栈把控制权交给 ELF 入口 `_start`：
//   [rsp]                 = argc (i32)
//   [rsp+8 ..]            = argv[0..argc]（指针数组，以 NULL 结尾）
//   [rsp+8+(argc+1)*8 ..] = envp[]（指针数组，以 NULL 结尾）
#[no_mangle]
pub unsafe extern "C" fn _start() -> ! {
    let sp: usize;
    core::arch::asm!("mov {}, rsp", out(reg) sp, options(nostack));
    let _argc = *(sp as *const i32);
    let _argv = sp.wrapping_add(8) as *const *const u8;
    let _envp = sp.wrapping_add(8).wrapping_add((_argc as usize + 1) * 8) as *const *const u8;
    // 注：std::env 的参数登记由 std 内部在运行时初始化阶段完成（sukios 后端经
    // native_set_args）；#![no_main] 下跳过 lang_start，此处仅读取栈布局备用。
    let _ = (_argv, _envp);
    std_test_main();
    std::process::exit(0);
}

fn std_test_main() {
    println!("[stdtest] hello from Rust std on SukiOS");

    let args: Vec<String> = std::env::args().collect();
    println!("[stdtest] args = {:?}", args);

    let v: Vec<u32> = (1..=10).collect();
    let sum: u32 = v.iter().sum();
    println!("[stdtest] Vec sum(1..=10) = {}", sum);

    let mut s = String::from("Suki");
    s.push_str("OS");
    println!("[stdtest] String = {} (len={})", s, s.len());

    let mut m = std::collections::HashMap::new();
    m.insert("os", "SukiOS");
    m.insert("arch", "x86_64");
    println!("[stdtest] HashMap[os] = {:?}", m.get("os"));

    let t = std::time::Instant::now();
    let mut acc = 0u64;
    for i in 0..100_000u64 {
        acc = acc.wrapping_add(i);
    }
    println!("[stdtest] loop acc={} took {:?}", acc, t.elapsed());

    println!("[stdtest] thread name = {:?}", std::thread::current().name());

    println!("[stdtest] PASS");
}
