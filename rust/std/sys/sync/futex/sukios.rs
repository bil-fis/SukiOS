//! `std::sys::sukios::sync::futex` 后端（SukiOS 原生实现，无 libc）。
//!
//! SukiOS 内核提供两个原生 futex 系统调用（SYS_SUKI_FUTEX_WAIT=218 /
//! SYS_SUKI_FUTEX_WAKE=219），语义与 Linux FUTEX_WAIT / FUTEX_WAKE 一致：
//!   * WAIT：仅当 `*uaddr == val` 时睡眠，否则立即返回；
//!   * WAKE：唤醒最多 `val` 个等待者（`val` 取极大值表示全部），返回被唤醒数。
//! 超时当前由内核忽略（P0 取舍：始终阻塞至被唤醒）。

use crate::sync::atomic::AtomicU32;
use crate::time::Duration;

use crate::sys::sukios_ffi as ffi;

// 供 mutex / condvar / once / rwlock 使用的类型别名（与上游 unix 后端一致）。
// `Futex = AtomicU32` 与 `Atomic<Primitive>`（`Primitive = u32`）等价。
pub type Primitive = u32;
pub type Futex = AtomicU32;
pub type SmallPrimitive = u32;
pub type SmallFutex = AtomicU32;

/// 等待 `futex_wake` 唤醒。若 futex 当前值 != expected 直接返回 true（不睡眠）；
/// 否则睡眠直到被唤醒，返回 true。本后端无超时语义，故不会返回 false。
pub fn futex_wait(futex: &AtomicU32, expected: u32, _timeout: Option<Duration>) -> bool {
    let r = unsafe {
        ffi::futex(
            futex as *const AtomicU32 as *mut u32,
            ffi::FUTEX_WAIT,
            expected,
            core::ptr::null::<ffi::c_void>(),
            core::ptr::null_mut(),
            0,
        )
    };
    r == 0
}

/// 唤醒一个等待者。返回是否确有等待者被唤醒。
pub fn futex_wake(futex: &AtomicU32) -> bool {
    let r = unsafe {
        ffi::futex(
            futex as *const AtomicU32 as *mut u32,
            ffi::FUTEX_WAKE,
            1,
            core::ptr::null::<ffi::c_void>(),
            core::ptr::null_mut(),
            0,
        )
    };
    r > 0
}

/// 唤醒所有等待者（`val` 取极大值）。
pub fn futex_wake_all(futex: &AtomicU32) {
    unsafe {
        ffi::futex(
            futex as *const AtomicU32 as *mut u32,
            ffi::FUTEX_WAKE,
            0x7fff_ffff,
            core::ptr::null::<ffi::c_void>(),
            core::ptr::null_mut(),
            0,
        );
    }
}
