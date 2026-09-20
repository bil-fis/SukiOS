//! `std::sys::sukios::random` 后端：经 SukiNative `SYS_SUKI_RANDOM` 填充随机字节。
//!
//! 内核提供 `getrandom(buf, len, flags)`（SYS_SUKI_RANDOM=217），返回实际填充的字节数，
//! 失败返回负值。本后端循环补齐直到填满；若内核随机源不可用则退化为零填充
//!（P0 取舍：仅熵质量下降，不致命，不影响 std 其余功能）。

use crate::sys::sukios_ffi::{getrandom, size_t};

/// 用密码学强度较弱的内核随机源填充 `bytes`。
pub fn fill_bytes(bytes: &mut [u8]) {
    if bytes.is_empty() {
        return;
    }
    let mut filled = 0usize;
    while filled < bytes.len() {
        let want = (bytes.len() - filled).min(isize::MAX as usize);
        let r = unsafe {
            getrandom(
                bytes[filled..].as_mut_ptr() as *mut _,
                want as size_t,
                0,
            )
        };
        if r <= 0 {
            // 内核随机源不可用：零填充兜底（不致命）。
            for b in &mut bytes[filled..] {
                *b = 0;
            }
            return;
        }
        filled += r as usize;
    }
}
