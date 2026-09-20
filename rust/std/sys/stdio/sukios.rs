//! SukiOS 标准 I/O 后端。
//!
//! 仅提供 stdin/stdout/stderr 三个文件描述符（0/1/2），写操作经
//! `crate::sys::sukios_ffi::write`（内核 SYS_DEBUG_WRITE）落到内核 serial。
//! SukiOS P0 不做终端行规/缓冲，flush 直接返回 Ok。

#[cfg(target_os = "hermit")]
use hermit_abi::{EBADF, STDERR_FILENO, STDIN_FILENO, STDOUT_FILENO};
#[cfg(any(any(target_family = "unix", target_os = "sukios"), target_os = "wasi"))]
use crate::sys::sukios_ffi::{EBADF, STDERR_FILENO, STDIN_FILENO, STDOUT_FILENO};

use crate::io::{self, BorrowedCursor, IoSlice, IoSliceMut};
use crate::mem::ManuallyDrop;
use crate::os::fd::FromRawFd;
use crate::sys::fd::FileDesc;

pub struct Stdin;
pub struct Stdout;
pub struct Stderr;

pub fn stdin() -> Stdin {
    Stdin
}

pub fn stdout() -> Stdout {
    Stdout
}

pub fn stderr() -> Stderr {
    Stderr
}

impl Stdin {
    pub const fn new() -> Stdin {
        Stdin
    }
}

impl io::Read for Stdin {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        unsafe { ManuallyDrop::new(FileDesc::from_raw_fd(STDIN_FILENO)).read(buf) }
    }

    fn read_buf(&mut self, buf: BorrowedCursor<'_, u8>) -> io::Result<()> {
        unsafe { ManuallyDrop::new(FileDesc::from_raw_fd(STDIN_FILENO)).read_buf(buf) }
    }

    fn read_vectored(&mut self, bufs: &mut [IoSliceMut<'_>]) -> io::Result<usize> {
        unsafe { ManuallyDrop::new(FileDesc::from_raw_fd(STDIN_FILENO)).read_vectored(bufs) }
    }

    #[inline]
    fn is_read_vectored(&self) -> bool {
        true
    }
}

impl Stdout {
    pub const fn new() -> Stdout {
        Stdout
    }
}

impl io::Write for Stdout {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        unsafe { ManuallyDrop::new(FileDesc::from_raw_fd(STDOUT_FILENO)).write(buf) }
    }

    fn write_vectored(&mut self, bufs: &[IoSlice<'_>]) -> io::Result<usize> {
        unsafe { ManuallyDrop::new(FileDesc::from_raw_fd(STDOUT_FILENO)).write_vectored(bufs) }
    }

    #[inline]
    fn is_write_vectored(&self) -> bool {
        true
    }

    #[inline]
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

impl Stderr {
    pub const fn new() -> Stderr {
        Stderr
    }
}

impl io::Write for Stderr {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        unsafe { ManuallyDrop::new(FileDesc::from_raw_fd(STDERR_FILENO)).write(buf) }
    }

    fn write_vectored(&mut self, bufs: &[IoSlice<'_>]) -> io::Result<usize> {
        unsafe { ManuallyDrop::new(FileDesc::from_raw_fd(STDERR_FILENO)).write_vectored(bufs) }
    }

    #[inline]
    fn is_write_vectored(&self) -> bool {
        true
    }

    #[inline]
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

pub fn is_ebadf(err: &io::Error) -> bool {
    err.raw_os_error() == Some(EBADF as i32)
}

pub const STDIN_BUF_SIZE: usize = crate::sys::io::DEFAULT_BUF_SIZE;

pub fn panic_output() -> Option<impl io::Write> {
    Some(Stderr::new())
}
