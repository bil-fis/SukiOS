// rust/std/sys/io/error/sukios.rs —— std `io::Error` 的 SukiOS 后端
// =============================================================================
// 提供四个入口（上游 std 的 `sys::io` 契约）：
//   errno()            读本线程最近一次失败的 errno（存在 SukiNative 层里）
//   is_interrupted()   是否 EINTR
//   decode_error_kind() errno → io::ErrorKind（映射表对照 include/sukios/posix.h）
//   format_error()     人类可读的错误描述（不依赖 C 的 strerror）
//
// 注意：errno 取自 `crate::sys::sukios_ffi`（我们的 SukiNative 层），**不经 libc**。
// =============================================================================
use crate::fmt;
use crate::sys::sukios_ffi as ffi;

#[inline]
pub fn errno() -> i32 {
    unsafe { *ffi::errno_location() as i32 }
}

#[inline]
pub fn is_interrupted(errno: i32) -> bool {
    errno == ffi::EINTR
}

pub fn decode_error_kind(errno: i32) -> crate::io::ErrorKind {
    use crate::io::ErrorKind::*;
    if errno == ffi::EAGAIN || errno == ffi::EWOULDBLOCK {
        return WouldBlock;
    }
    if errno == ffi::ENOTSUP || errno == 95 {
        return Unsupported;
    }
    match errno {
        ffi::E2BIG => ArgumentListTooLong,
        ffi::EACCES | ffi::EPERM => PermissionDenied,
        ffi::EADDRINUSE => AddrInUse,
        ffi::EBUSY => ResourceBusy,
        ffi::ECONNRESET => ConnectionReset,
        ffi::EEXIST => AlreadyExists,
        ffi::EFBIG => FileTooLarge,
        ffi::EINTR => Interrupted,
        ffi::EINVAL => InvalidInput,
        ffi::EISDIR => IsADirectory,
        ffi::ELOOP => FilesystemLoop,
        ffi::EMFILE | ffi::ENFILE => TooManyOpenFiles,
        ffi::ENAMETOOLONG => InvalidFilename,
        ffi::ENOENT => NotFound,
        ffi::ENOMEM => OutOfMemory,
        ffi::ENOSPC => StorageFull,
        ffi::ENOSYS => Unsupported,
        ffi::ENOTDIR => NotADirectory,
        ffi::ENOTEMPTY => DirectoryNotEmpty,
        ffi::EPIPE => BrokenPipe,
        ffi::EROFS => ReadOnlyFilesystem,
        ffi::ESPIPE => NotSeekable,
        ffi::EIO => InputOutputError,
        _ => Uncategorized,
    }
}

/// 错误号 → 可读描述（`strerror` 的替代：不依赖 C 运行时，表内自带中文/英文名词）。
pub fn format_error(errno: i32, f: &mut fmt::Formatter<'_>) -> fmt::Result {
    let s = match errno {
        0 => "success",
        ffi::EPERM => "operation not permitted",
        ffi::ENOENT => "no such file or directory",
        ffi::ESRCH => "no such process",
        ffi::EINTR => "interrupted system call",
        ffi::EIO => "input/output error",
        ffi::ENXIO => "no such device or address",
        ffi::E2BIG => "argument list too long",
        ffi::ENOEXEC => "exec format error",
        ffi::EBADF => "bad file descriptor",
        ffi::ECHILD => "no child processes",
        ffi::EAGAIN => "resource temporarily unavailable",
        ffi::ENOMEM => "cannot allocate memory",
        ffi::EACCES => "permission denied",
        ffi::EFAULT => "bad address",
        ffi::EBUSY => "device or resource busy",
        ffi::EEXIST => "file exists",
        ffi::EXDEV => "invalid cross-device link",
        ffi::ENODEV => "no such device",
        ffi::ENOTDIR => "not a directory",
        ffi::EISDIR => "is a directory",
        ffi::EINVAL => "invalid argument",
        ffi::ENFILE => "too many open files in system",
        ffi::EMFILE => "too many open files",
        ffi::ENOTTY => "inappropriate ioctl for device",
        ffi::EFBIG => "file too large",
        ffi::ENOSPC => "no space left on device",
        ffi::ESPIPE => "illegal seek",
        ffi::EROFS => "read-only file system",
        ffi::EPIPE => "broken pipe",
        ffi::ERANGE => "numerical result out of range",
        ffi::ENAMETOOLONG => "file name too long",
        ffi::ENOSYS => "function not implemented",
        ffi::ENOTEMPTY => "directory not empty",
        ffi::ELOOP => "too many levels of symbolic links",
        -1 => "unknown error",
        _ => return write!(f, "unknown error ({})", errno),
    };
    f.write_str(s)
}
