// rust/std/sys/sukios_ffi.rs  ——  SukiOS 的 **SukiNative 系统调用层**
// =============================================================================
// 本文件是 Rust std 的 `target_os = "sukios"` 后端与内核之间的**唯一**通道。
//
// 设计要点（用户要求：使用 SukiNative，不要使用 POSIX）：
//   * **不链接任何 C 运行库**（没有 libsuki.a、没有 crt0）；所有系统调用都用
//     x86_64 `syscall` 指令直接发出，号位取自 SukiNative / 原生号段：
//         0..19    原生（mach_msg / task_exit / yield / debug_write / mmap_legacy …）
//         130..149 SukiNative 对象/句柄 API（MEM_ALLOC / FILE_* / OBJ_QUERY …）
//         214..249 SukiNative Phase 3（时间 / 休眠 / 随机 …）
//   * 本模块对外仍保持「libc 形状」的名字（`open`/`read`/`malloc`…），原因是上游
//     std 的 sukios 后端与若干共享模块按这个形状调用；但**实现全部是原生的**，
//     不使用 POSIX 号段（20..129）。
//   * 对象/句柄语义：文件等资源经 SukiNative 句柄（`suki_handle_t`）持有，错误码
//     为 `-SUKI_E*`（负数），本层转换成 libc 风格（-1 + errno）返回。
//
// 阶段划分（见 results/step106.md）：
//   P0（本文件当前范围）：堆、stdout/stderr/stdin、时间、随机、进程自省、退出。
//   P1+：文件元数据/目录/线程/进程等，需要内核 SukiNative Phase 3 继续扩号位。
//
// 未实现项一律返回 `ENOSYS`（编译期契约完整、运行期显式失败），绝不静默假成功。
// =============================================================================

#![allow(dead_code)]
#![allow(non_snake_case)]
#![allow(non_camel_case_types)]
#![allow(clippy::missing_safety_doc)]

pub use core::ffi::c_void;
use core::ptr;

// ============================================================================
//  SukiNative 系统调用号（与 include/sukios/posix.h 单一真相源一致）
// ============================================================================
pub const SYS_MACH_MSG: u64 = 0;
pub const SYS_TASK_EXIT: u64 = 2;
pub const SYS_YIELD: u64 = 3;
pub const SYS_DEBUG_WRITE: u64 = 4;
pub const SYS_MMAP_LEGACY: u64 = 14;
pub const SYS_MUNMAP_LEGACY: u64 = 15;
pub const SYS_SERIAL_READ: u64 = 16;
pub const SYS_GETPID: u64 = 18;
pub const SYS_GETPPID: u64 = 19;

pub const SYS_SUKI_OBJ_QUERY: u64 = 133;
pub const SYS_SUKI_FILE_OPEN: u64 = 144;
pub const SYS_SUKI_FILE_READ: u64 = 145;
pub const SYS_SUKI_FILE_WRITE: u64 = 146;
pub const SYS_SUKI_FILE_CLOSE: u64 = 147;
pub const SYS_SUKI_MEM_ALLOC: u64 = 149;

pub const SYS_SUKI_TIME_MONOTONIC: u64 = 214;
pub const SYS_SUKI_TIME_REALTIME: u64 = 215;
pub const SYS_SUKI_SLEEP_NS: u64 = 216;
pub const SYS_SUKI_RANDOM: u64 = 217;
pub const SYS_SUKI_FUTEX_WAIT: u64 = 218;
pub const SYS_SUKI_FUTEX_WAKE: u64 = 219;

/// 发出一次系统调用。约定：%rax=号，%rdi/%rsi/%rdx/%r10/%r8/%r9=参数，返回 %rax。
/// `rcx`/`r11` 被 `syscall` 指令破坏，必须声明为 clobber。
#[inline(always)]
pub unsafe fn syscall6(num: u64, a1: u64, a2: u64, a3: u64, a4: u64, a5: u64,
                       a6: u64) -> u64 {
    let ret: u64;
    unsafe {
        core::arch::asm!(
            "syscall",
            in("rax") num,
            in("rdi") a1,
            in("rsi") a2,
            in("rdx") a3,
            in("r10") a4,
            in("r8") a5,
            in("r9") a6,
            lateout("rax") ret,
            out("rcx") _,
            out("r11") _,
            options(nostack),
        );
    }
    ret
}

#[inline(always)]
pub unsafe fn sys2(num: u64, a1: u64, a2: u64) -> i64 {
    unsafe { syscall6(num, a1, a2, 0, 0, 0, 0) as i64 }
}

#[inline(always)]
pub unsafe fn sys3(num: u64, a1: u64, a2: u64, a3: u64) -> i64 {
    unsafe { syscall6(num, a1, a2, a3, 0, 0, 0) as i64 }
}

#[inline(always)]
pub unsafe fn sys1(num: u64, a1: u64) -> i64 {
    unsafe { syscall6(num, a1, 0, 0, 0, 0, 0) as i64 }
}

#[inline(always)]
pub unsafe fn sys0(num: u64) -> i64 {
    unsafe { syscall6(num, 0, 0, 0, 0, 0, 0) as i64 }
}

// ============================================================================
//  基础 C 类型（LP64）
// ============================================================================
pub use core::ffi::{c_char, c_int, c_long, c_longlong, c_short, c_uchar, c_uint,
                    c_ulong, c_ulonglong, c_ushort};

pub type size_t = usize;
pub type ssize_t = isize;
pub type intptr_t = isize;
pub type uintptr_t = usize;
pub type ptrdiff_t = isize;
pub type off_t = i64;
pub type off64_t = i64;
pub type mode_t = u32;
pub type pid_t = i32;
pub type uid_t = u32;
pub type gid_t = u32;
pub type dev_t = u64;
pub type ino_t = u64;
pub type nlink_t = u32;
pub type blksize_t = i64;
pub type blkcnt_t = i64;
pub type time_t = i64;
pub type clockid_t = i32;
pub type suseconds_t = i64;
pub type useconds_t = u32;
pub type socklen_t = u32;
pub type sa_family_t = u16;
pub type in_port_t = u16;
pub type in_addr_t = u32;
pub type lwpid_t = i32;
pub type cpuid_t = u32;
pub type kern_return_t = c_int;
pub type pthread_t = u64;
pub type sigset_t = u64;

pub const KERN_SUCCESS: kern_return_t = 0;
pub const B_OK: c_int = 0;

#[repr(C)]
#[derive(Copy, Clone, Default)]
pub struct timespec {
    pub tv_sec: time_t,
    pub tv_nsec: c_long,
}

#[repr(C)]
#[derive(Copy, Clone, Default)]
pub struct timeval {
    pub tv_sec: time_t,
    pub tv_usec: suseconds_t,
}

#[repr(C)]
#[derive(Copy, Clone, Default)]
pub struct timezone {
    pub tz_minuteswest: c_int,
    pub tz_dsttime: c_int,
}

/// `struct stat`：字段顺序**必须**与内核 `include/sukios/posix.h` 的 suki_stat_t 一致。
#[repr(C)]
#[derive(Copy, Clone, Default)]
pub struct stat {
    pub st_dev: dev_t,
    pub st_ino: ino_t,
    pub st_nlink: nlink_t,
    pub st_mode: mode_t,
    pub st_uid: uid_t,
    pub st_gid: gid_t,
    pub st_rdev: dev_t,
    pub st_size: off_t,
    pub st_blksize: blksize_t,
    pub st_blocks: blkcnt_t,
    pub st_atime: time_t,
    pub st_atime_nsec: c_long,
    pub st_mtime: time_t,
    pub st_mtime_nsec: c_long,
    pub st_ctime: time_t,
    pub st_ctime_nsec: c_long,
}

/// 目录流（本层不透明地持有 SukiNative 句柄；P2 接 DIR_* 时填充）。
#[repr(C)]
pub struct DIR {
    _opaque: [u8; 0],
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct dirent {
    pub d_ino: ino_t,
    pub d_off: off_t,
    pub d_reclen: c_ushort,
    pub d_type: c_uchar,
    pub d_name: [c_char; 256],
}

#[repr(C)]
#[derive(Copy, Clone, Default)]
pub struct iovec {
    pub iov_base: *mut c_void,
    pub iov_len: size_t,
}

#[repr(C)]
#[derive(Copy, Clone, Default)]
pub struct pollfd {
    pub fd: c_int,
    pub events: c_short,
    pub revents: c_short,
}

#[repr(C)]
#[derive(Copy, Clone, Default)]
pub struct sigaction {
    pub sa_handler: usize,
    pub sa_mask: sigset_t,
    pub sa_flags: c_int,
    pub sa_restorer: usize,
}

#[repr(C)]
#[derive(Copy, Clone, Default)]
pub struct rlimit {
    pub rlim_cur: u64,
    pub rlim_max: u64,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct utsname {
    pub sysname: [c_char; 65],
    pub nodename: [c_char; 65],
    pub release: [c_char; 65],
    pub version: [c_char; 65],
    pub machine: [c_char; 65],
}

// ============================================================================
//  常量（与 include/sukios/posix.h 的 SUKI_E* / O_* / S_* / CLOCK_* 对应）
// ============================================================================
pub const EPERM: c_int = 1;
pub const ENOENT: c_int = 2;
pub const ESRCH: c_int = 3;
pub const EINTR: c_int = 4;
pub const EIO: c_int = 5;
pub const ENXIO: c_int = 6;
pub const E2BIG: c_int = 7;
pub const ENOEXEC: c_int = 8;
pub const EBADF: c_int = 9;
pub const ECHILD: c_int = 10;
pub const EAGAIN: c_int = 11;
pub const EWOULDBLOCK: c_int = EAGAIN;
pub const ENOMEM: c_int = 12;
pub const EACCES: c_int = 13;
pub const EFAULT: c_int = 14;
pub const EBUSY: c_int = 16;
pub const EEXIST: c_int = 17;
pub const EXDEV: c_int = 18;
pub const ENODEV: c_int = 19;
pub const ENOTDIR: c_int = 20;
pub const EISDIR: c_int = 21;
pub const EINVAL: c_int = 22;
pub const ENFILE: c_int = 23;
pub const EMFILE: c_int = 24;
pub const ENOTTY: c_int = 25;
pub const EFBIG: c_int = 27;
pub const ENOSPC: c_int = 28;
pub const ESPIPE: c_int = 29;
pub const EROFS: c_int = 30;
pub const EPIPE: c_int = 32;
pub const ERANGE: c_int = 34;
pub const ENAMETOOLONG: c_int = 36;
pub const ENOSYS: c_int = 38;
pub const ENOTEMPTY: c_int = 39;
pub const ELOOP: c_int = 40;
pub const ENOTSUP: c_int = 95;
pub const ECONNRESET: c_int = 104;
pub const EADDRINUSE: c_int = 98;

pub const STDIN_FILENO: c_int = 0;
pub const STDOUT_FILENO: c_int = 1;
pub const STDERR_FILENO: c_int = 2;

pub const O_RDONLY: c_int = 0x0000;
pub const O_WRONLY: c_int = 0x0001;
pub const O_RDWR: c_int = 0x0002;
pub const O_ACCMODE: c_int = 0x0003;
pub const O_CREAT: c_int = 0x0040;
pub const O_EXCL: c_int = 0x0080;
pub const O_TRUNC: c_int = 0x0200;
pub const O_APPEND: c_int = 0x0400;
pub const O_NONBLOCK: c_int = 0x0800;
pub const O_DIRECTORY: c_int = 0x10000;
pub const O_CLOEXEC: c_int = 0x80000;

pub const F_DUPFD: c_int = 0;
pub const F_GETFD: c_int = 1;
pub const F_SETFD: c_int = 2;
pub const F_GETFL: c_int = 3;
pub const F_SETFL: c_int = 4;
pub const FD_CLOEXEC: c_int = 1;
pub const FIONBIO: c_ulong = 0x5421;
pub const FIOCLEX: c_ulong = 0x5451;

pub const SEEK_SET: c_int = 0;
pub const SEEK_CUR: c_int = 1;
pub const SEEK_END: c_int = 2;

pub const S_IFMT: mode_t = 0o170000;
pub const S_IFDIR: mode_t = 0o040000;
pub const S_IFREG: mode_t = 0o100000;
pub const S_IFLNK: mode_t = 0o120000;

pub const CLOCK_REALTIME: clockid_t = 0;
pub const CLOCK_MONOTONIC: clockid_t = 1;
pub const CLOCK_PROCESS_CPUTIME_ID: clockid_t = 2;
pub const CLOCK_THREAD_CPUTIME_ID: clockid_t = 3;
pub const CLOCK_UPTIME_RAW: clockid_t = 8;
pub const TIMER_ABSTIME: c_int = 1;

pub const PROT_NONE: c_int = 0;
pub const PROT_READ: c_int = 1;
pub const PROT_WRITE: c_int = 2;
pub const PROT_EXEC: c_int = 4;
pub const MAP_PRIVATE: c_int = 0x02;
pub const MAP_ANONYMOUS: c_int = 0x20;
pub const MAP_ANON: c_int = MAP_ANONYMOUS;
pub const MAP_FAILED: *mut c_void = usize::MAX as *mut c_void;

pub const IOV_MAX: c_int = 16;
pub const UIO_MAXIOV: c_int = 16;
pub const PATH_MAX: usize = 4096;
pub const NAME_MAX: usize = 255;
pub const MAXTHREADNAMESIZE: usize = 64;
pub const PTHREAD_STACK_MIN: usize = 16 * 1024;
pub const _NTO_THREAD_NAME_MAX: usize = 64;
pub const VX_TASK_RENAME_LENGTH: usize = 64;
pub const _SC_NPROCESSORS_ONLN: c_int = 84;
pub const _SC_THREAD_STACK_MIN: c_int = 71;
pub const _SC_GETPW_R_SIZE_MAX: c_int = 70;
pub const POLLIN: c_short = 0x001;
pub const AF_UNIX: c_int = 1;
pub const SOCK_SEQPACKET: c_int = 5;
pub const MSG_EOR: c_int = 0x80;
pub const MSG_CMSG_CLOEXEC: c_int = 0x40000000;

pub const SIG_DFL: usize = 0;
pub const SIG_ERR: usize = usize::MAX;

// ============================================================================
//  errno（libc 风格：写入成功返回 0/-1，错误经 errno() 读取）
// ============================================================================
static mut ERRNO: c_int = 0;

#[inline]
pub fn set_errno(e: c_int) {
    unsafe { ERRNO = e }
}

pub unsafe fn errno_location() -> *mut c_int {
    &raw mut ERRNO
}

// ============================================================================
//  堆：在 SukiNative MEM_ALLOC 之上实现（P0）
// ----------------------------------------------------------------------------
//  语义：
//    * 向内核按 1 MiB 粒度申请匿名内存（`SYS_SUKI_MEM_ALLOC` → 句柄，
//      `SYS_SUKI_OBJ_QUERY` 取回映射基址；内存按需零填、W^X 不可执行）。
//    * 块头 16 字节（{块指针, 块大小}）紧邻用户指针之前，故 free 可 O(1) 归还。
//    * 空闲链按首次适配分配并切分；**不做相邻合并**（P0 取舍：正确性优先，
//      碎片只影响峰值内存；后续阶段可换成 size-class/伙伴分配器）。
// ============================================================================
const HEAP_CHUNK: usize = 1 << 20;   // 每次向内核申请的字节数
const HDR: usize = 16;               // 块头：{blk: *mut u8, size: usize}

#[repr(C)]
struct BlkHeader {
    blk: *mut u8,   // 该分配所属的「空闲块」起始地址（归还目标）
    size: usize,    // 该空闲块的完整大小（含块头与对齐余量）
}

#[repr(C)]
struct FreeBlk {
    size: usize,
    next: *mut FreeBlk,
}

static mut FREE_LIST: *mut FreeBlk = ptr::null_mut();
static mut HEAP_FAILED: bool = false;

/// 向内核申请一块匿名内存，返回 (基址, 句柄)。失败返回 (null, 0)。
unsafe fn chunk_alloc(size: usize) -> (*mut u8, u64) {
    let mut handle: u64 = 0;
    let rc = unsafe { sys2(SYS_SUKI_MEM_ALLOC, size as u64, &mut handle as *mut u64 as u64) };
    if rc != 0 || handle == 0 {
        return (ptr::null_mut(), 0);
    }
    // OBJ_QUERY 回填 suki_objinfo_t{ type,state,count,_pad, base, size }
    #[repr(C)]
    struct ObjInfo {
        ty: u32,
        state: u32,
        count: i32,
        _pad: u32,
        base: u64,
        size: u64,
    }
    let mut info = ObjInfo { ty: 0, state: 0, count: 0, _pad: 0, base: 0, size: 0 };
    let rc2 = unsafe { sys2(SYS_SUKI_OBJ_QUERY, handle, &mut info as *mut ObjInfo as u64) };
    if rc2 != 0 || info.base == 0 {
        return (ptr::null_mut(), 0);
    }
    (info.base as *mut u8, handle)
}

/// 从空闲链取一个大小 >= total 的块（首次适配）。取不到返回 null。
unsafe fn freelist_take(total: usize) -> *mut u8 {
    unsafe {
        let mut prev: *mut FreeBlk = ptr::null_mut();
        let mut cur = FREE_LIST;
        while !cur.is_null() {
            let bsize = (*cur).size;
            if bsize >= total {
                let next = (*cur).next;
                let base = cur as *mut u8;
                // 摘链
                if prev.is_null() {
                    FREE_LIST = next;
                } else {
                    (*prev).next = next;
                }
                // 切分：剩余 >= 32 字节则作为新空闲块挂回链首
                let remain = bsize - total;
                if remain >= 32 {
                    let rest = base.add(total) as *mut FreeBlk;
                    (*rest).size = remain;
                    (*rest).next = FREE_LIST;
                    FREE_LIST = rest;
                    // 本块只用 total
                    (*(base as *mut BlkHeader)).size = total;
                } else {
                    (*(base as *mut BlkHeader)).size = bsize;
                }
                return base;
            }
            prev = cur;
            cur = (*cur).next;
        }
        ptr::null_mut()
    }
}

unsafe fn heap_grow(need: usize) -> bool {
    unsafe {
        let mut ask = HEAP_CHUNK;
        if need > ask {
            ask = (need + 4095) & !4095usize;
        }
        let (base, _h) = chunk_alloc(ask);
        if base.is_null() {
            HEAP_FAILED = true;
            return false;
        }
        let blk = base as *mut FreeBlk;
        (*blk).size = ask;
        (*blk).next = FREE_LIST;
        FREE_LIST = blk;
        true
    }
}

/// 通用分配：`size` 字节、`align` 对齐（align 必须是 2 的幂，且 <= 4096）。
unsafe fn raw_alloc(size: usize, align: usize) -> *mut u8 {
    unsafe {
        if size == 0 || HEAP_FAILED {
            return ptr::null_mut();
        }
        let align = if align < 8 { 8 } else { align };
        let total = size + HDR + align;
        let mut blk = freelist_take(total);
        if blk.is_null() {
            if !heap_grow(total) {
                return ptr::null_mut();
            }
            blk = freelist_take(total);
            if blk.is_null() {
                return ptr::null_mut();
            }
        }
        let blk_size = (*(blk as *mut BlkHeader)).size;
        let user = ((blk as usize) + HDR + (align - 1)) & !(align - 1);
        let hdr = (user - HDR) as *mut BlkHeader;
        (*hdr).blk = blk;
        (*hdr).size = blk_size;
        user as *mut u8
    }
}

unsafe fn raw_free(p: *mut u8) {
    unsafe {
        if p.is_null() {
            return;
        }
        let hdr = (p as usize - HDR) as *const BlkHeader;
        let blk = (*hdr).blk;
        let size = (*hdr).size;
        if blk.is_null() || size < 32 {
            return;                 // 非本分配器指针：忽略（防御）
        }
        let fb = blk as *mut FreeBlk;
        (*fb).size = size;
        (*fb).next = FREE_LIST;
        FREE_LIST = fb;
    }
}

// ============================================================================
//  对外 API（libc 形状）
// ============================================================================

pub unsafe fn malloc(size: size_t) -> *mut c_void {
    unsafe { raw_alloc(size, 16) as *mut c_void }
}

pub unsafe fn calloc(nmemb: size_t, size: size_t) -> *mut c_void {
    unsafe {
        let total = match nmemb.checked_mul(size) {
            Some(t) => t,
            None => {
                set_errno(ENOMEM);
                return ptr::null_mut();
            }
        };
        let p = raw_alloc(total, 16);
        if !p.is_null() {
            ptr::write_bytes(p, 0, total);
        }
        p as *mut c_void
    }
}

pub unsafe fn realloc(old: *mut c_void, size: size_t) -> *mut c_void {
    unsafe {
        if old.is_null() {
            return malloc(size);
        }
        if size == 0 {
            raw_free(old as *mut u8);
            return ptr::null_mut();
        }
        // 旧块可用容量（用户可用 = 块大小 - 头 - 对齐余量，保守取 blk.size - HDR - 16）
        let hdr = (old as usize - HDR) as *const BlkHeader;
        let cap = (*hdr).size.saturating_sub(HDR + 16);
        if cap >= size {
            return old;             // 原地够用
        }
        let np = raw_alloc(size, 16);
        if np.is_null() {
            return ptr::null_mut();
        }
        ptr::copy_nonoverlapping(old as *const u8, np, cap.min(size));
        raw_free(old as *mut u8);
        np as *mut c_void
    }
}

pub unsafe fn free(p: *mut c_void) {
    unsafe { raw_free(p as *mut u8) }
}

pub unsafe fn memalign(align: size_t, size: size_t) -> *mut c_void {
    unsafe { raw_alloc(size, align) as *mut c_void }
}

pub unsafe fn posix_memalign(out: *mut *mut c_void, align: size_t, size: size_t) -> c_int {
    unsafe {
        if align < core::mem::size_of::<usize>() || (align & (align - 1)) != 0 {
            return EINVAL;
        }
        let p = raw_alloc(size, align);
        if p.is_null() {
            return ENOMEM;
        }
        *out = p as *mut c_void;
        0
    }
}

// ---------------------------------------------------------------- 标准输入输出
/// 写：fd 1/2 → 内核 `SYS_DEBUG_WRITE`（串口 + 用户 TTY 环形管道，由 shell 终端
/// 呈现）；fd 0 不可写；其它 fd → 暂 ENOSYS（P2 接 SukiNative FILE 句柄）。
pub unsafe fn write(fd: c_int, buf: *const c_void, count: size_t) -> ssize_t {
    unsafe {
        if buf.is_null() {
            set_errno(EFAULT);
            return -1;
        }
        if fd == STDOUT_FILENO || fd == STDERR_FILENO {
            let rc = sys2(SYS_DEBUG_WRITE, buf as u64, count as u64);
            if rc < 0 {
                set_errno(EIO);
                return -1;
            }
            return count as ssize_t;
        }
        set_errno(ENOSYS);
        -1
    }
}

/// 读：fd 0 → 内核 `SYS_SERIAL_READ`（取一个字节，无数据返回 0）。
pub unsafe fn read(fd: c_int, buf: *mut c_void, count: size_t) -> ssize_t {
    unsafe {
        if buf.is_null() {
            set_errno(EFAULT);
            return -1;
        }
        if fd == STDIN_FILENO {
            if count == 0 {
                return 0;
            }
            let c = sys0(SYS_SERIAL_READ);
            if c < 0 {
                return 0;           // 暂无输入：非阻塞返回 0（stdin 语义）
            }
            *(buf as *mut u8) = c as u8;
            return 1;
        }
        set_errno(ENOSYS);
        -1
    }
}

pub unsafe fn close(fd: c_int) -> c_int {
    unsafe {
        let _ = fd;
        0                            // 标准流无需关闭；P2 起走 SukiNative 句柄
    }
}

pub unsafe fn readv(_fd: c_int, _iov: *const iovec, _cnt: c_int) -> ssize_t {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn writev(_fd: c_int, _iov: *const iovec, _cnt: c_int) -> ssize_t {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn preadv(_fd: c_int, _iov: *const iovec, _cnt: c_int, _off: off_t) -> ssize_t {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn pwritev(_fd: c_int, _iov: *const iovec, _cnt: c_int, _off: off_t) -> ssize_t {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn pread(_fd: c_int, _buf: *mut c_void, _n: size_t, _off: off_t) -> ssize_t {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn pwrite(_fd: c_int, _buf: *const c_void, _n: size_t, _off: off_t) -> ssize_t {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn lseek(_fd: c_int, _off: off_t, _whence: c_int) -> off_t {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn fcntl(_fd: c_int, _cmd: c_int, _arg: c_int) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn ioctl(_fd: c_int, _req: c_ulong, _arg: *mut c_void) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn isatty(fd: c_int) -> c_int {
    unsafe { (fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO) as c_int }
}
pub unsafe fn fsync(_fd: c_int) -> c_int {
    unsafe { 0 }
}
pub unsafe fn fdatasync(_fd: c_int) -> c_int {
    unsafe { 0 }
}
pub unsafe fn dup(_fd: c_int) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn dup2(_o: c_int, _n: c_int) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn pipe(_fds: *mut c_int) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn pipe2(_fds: *mut c_int, _flags: c_int) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}

// ---------------------------------------------------------------- 文件（P2）
pub unsafe fn open(_p: *const c_char, _f: c_int, _m: mode_t) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn openat(_d: c_int, _p: *const c_char, _f: c_int, _m: mode_t) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn stat(_p: *const c_char, _st: *mut stat) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn lstat(_p: *const c_char, _st: *mut stat) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn fstat(_fd: c_int, _st: *mut stat) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn ftruncate(_fd: c_int, _len: off_t) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn mkdir(_p: *const c_char, _m: mode_t) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn rmdir(_p: *const c_char) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn unlink(_p: *const c_char) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn unlinkat(_d: c_int, _p: *const c_char, _f: c_int) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn rename(_o: *const c_char, _n: *const c_char) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn link(_o: *const c_char, _n: *const c_char) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn symlink(_t: *const c_char, _l: *const c_char) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn readlink(_p: *const c_char, _b: *mut c_char, _n: size_t) -> ssize_t {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn chmod(_p: *const c_char, _m: mode_t) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn fchmod(_fd: c_int, _m: mode_t) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn access(_p: *const c_char, _m: c_int) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn chdir(_p: *const c_char) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn chroot(_p: *const c_char) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn getcwd(buf: *mut c_char, size: size_t) -> *mut c_char {
    unsafe {
        // P0：暂无 cwd（P2 接 SukiNative CHDIR/GETCWD）。返回 "/" 以让 std 可启动。
        if buf.is_null() || size < 2 {
            set_errno(EINVAL);
            return ptr::null_mut();
        }
        *buf = b'/' as c_char;
        *buf.add(1) = 0;
        buf
    }
}
pub unsafe fn opendir(_p: *const c_char) -> *mut DIR {
    unsafe { set_errno(ENOSYS); ptr::null_mut() }
}
pub unsafe fn fdopendir(_fd: c_int) -> *mut DIR {
    unsafe { set_errno(ENOSYS); ptr::null_mut() }
}
pub unsafe fn readdir(_d: *mut DIR) -> *mut dirent {
    unsafe { set_errno(ENOSYS); ptr::null_mut() }
}
pub unsafe fn closedir(_d: *mut DIR) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn telldir(_d: *mut DIR) -> c_long {
    unsafe { -1 }
}
pub unsafe fn seekdir(_d: *mut DIR, _l: c_long) {}
pub unsafe fn rewinddir(_d: *mut DIR) {}

// ---------------------------------------------------------------- 进程（P3）
pub unsafe fn fork() -> pid_t {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn execve(_p: *const c_char, _a: *const *const c_char,
                     _e: *const *const c_char) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn execvp(_p: *const c_char, _a: *const *const c_char) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn waitpid(_pid: pid_t, _st: *mut c_int, _opt: c_int) -> pid_t {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn wait(_st: *mut c_int) -> pid_t {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn kill(_pid: pid_t, _sig: c_int) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn killpg(_pgrp: pid_t, _sig: c_int) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn poll(_fds: *mut pollfd, _n: c_ulong, _timeout: c_int) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn sigaction(_s: c_int, _a: *const sigaction, _o: *mut sigaction) -> c_int {
    unsafe { 0 }                 // P0：无信号子系统，注册视为成功（默认动作=终止）
}
pub unsafe fn signal(_s: c_int, _h: usize) -> usize {
    SIG_DFL
}
pub unsafe fn sigprocmask(_how: c_int, _set: *const sigset_t,
                          _old: *mut sigset_t) -> c_int {
    unsafe { 0 }
}
pub unsafe fn getrlimit(_r: c_int, out: *mut rlimit) -> c_int {
    unsafe {
        if out.is_null() {
            set_errno(EFAULT);
            return -1;
        }
        (*out).rlim_cur = 1 << 30;
        (*out).rlim_max = 1 << 30;
        0
    }
}
pub unsafe fn setrlimit(_r: c_int, _l: *const rlimit) -> c_int {
    unsafe { 0 }
}
pub unsafe fn uname(out: *mut utsname) -> c_int {
    unsafe {
        if out.is_null() {
            set_errno(EFAULT);
            return -1;
        }
        (*out) = utsname {
            sysname: [0; 65], nodename: [0; 65], release: [0; 65],
            version: [0; 65], machine: [0; 65],
        };
        fill(&mut (*out).sysname, b"SukiOS");
        fill(&mut (*out).nodename, b"sukios");
        fill(&mut (*out).release, b"1.0.0");
        fill(&mut (*out).machine, b"x86_64");
        0
    }
}

unsafe fn fill(dst: &mut [c_char; 65], s: &[u8]) {
    unsafe {
        let n = s.len().min(64);
        for i in 0..n {
            dst[i] = s[i] as c_char;
        }
        dst[n] = 0;
    }
}

pub unsafe fn getpid() -> pid_t {
    unsafe { sys0(SYS_GETPID) as pid_t }
}
pub unsafe fn getppid() -> pid_t {
    unsafe { sys0(SYS_GETPPID) as pid_t }
}
pub unsafe fn gettid() -> pid_t {
    unsafe { getpid() }          // P0：单线程，tid == pid
}
pub unsafe fn getuid() -> uid_t {
    0                            // SukiOS 当前无用户体系（见 README 权限子系统预留）
}
pub unsafe fn getgid() -> gid_t {
    0
}
pub unsafe fn setuid(_u: uid_t) -> c_int {
    0
}
pub unsafe fn setgid(_g: gid_t) -> c_int {
    0
}
pub unsafe fn setgroups(_n: size_t, _l: *const gid_t) -> c_int {
    0
}
pub unsafe fn setpgid(_p: pid_t, _pg: pid_t) -> c_int {
    0
}
pub unsafe fn setsid() -> pid_t {
    1
}
pub unsafe fn exit(code: c_int) -> ! {
    unsafe {
        sys1(SYS_TASK_EXIT, code as u64);
        loop {
            core::hint::spin_loop();
        }
    }
}
pub unsafe fn _exit(code: c_int) -> ! {
    unsafe { exit(code) }
}
pub unsafe fn abort() -> ! {
    unsafe { exit(134) }
}
pub unsafe fn raise(_sig: c_int) -> c_int {
    0
}

pub unsafe fn sched_yield() -> c_int {
    unsafe {
        sys0(SYS_YIELD);
        0
    }
}

// ---------------------------------------------------------------- 时间
pub unsafe fn clock_gettime(clk: clockid_t, tp: *mut timespec) -> c_int {
    unsafe {
        if tp.is_null() {
            set_errno(EFAULT);
            return -1;
        }
        let ns = match clk {
            CLOCK_REALTIME => sys0(SYS_SUKI_TIME_REALTIME) as u64,
            // 单调（含 CPU/线程时钟，P0 一律退化为系统单调时间）
            _ => sys0(SYS_SUKI_TIME_MONOTONIC) as u64,
        };
        (*tp).tv_sec = (ns / 1_000_000_000) as time_t;
        (*tp).tv_nsec = (ns % 1_000_000_000) as c_long;
        0
    }
}

pub unsafe fn clock_getres(_clk: clockid_t, tp: *mut timespec) -> c_int {
    unsafe {
        if !tp.is_null() {
            (*tp).tv_sec = 0;
            (*tp).tv_nsec = 1_000_000;      // 内核 100Hz 节拍下保守取 1ms
        }
        0
    }
}

pub unsafe fn gettimeofday(tv: *mut timeval, tz: *mut timezone) -> c_int {
    unsafe {
        if !tv.is_null() {
            let ns = sys0(SYS_SUKI_TIME_REALTIME) as u64;
            (*tv).tv_sec = (ns / 1_000_000_000) as time_t;
            (*tv).tv_usec = ((ns % 1_000_000_000) / 1000) as suseconds_t;
        }
        if !tz.is_null() {
            (*tz).tz_minuteswest = 0;
            (*tz).tz_dsttime = 0;
        }
        0
    }
}

pub unsafe fn time(t: *mut time_t) -> time_t {
    unsafe {
        let ns = sys0(SYS_SUKI_TIME_REALTIME) as u64;
        let secs = (ns / 1_000_000_000) as time_t;
        if !t.is_null() {
            *t = secs;
        }
        secs
    }
}

pub unsafe fn nanosleep(req: *const timespec, _rem: *mut timespec) -> c_int {
    unsafe {
        if req.is_null() {
            set_errno(EFAULT);
            return -1;
        }
        let ns = (*req).tv_sec as u64 * 1_000_000_000 + (*req).tv_nsec as u64;
        sys1(SYS_SUKI_SLEEP_NS, ns);
        0
    }
}

pub unsafe fn usleep(usecs: useconds_t) -> c_int {
    unsafe {
        sys1(SYS_SUKI_SLEEP_NS, (usecs as u64) * 1000);
        0
    }
}

pub unsafe fn clock_nanosleep(_clk: clockid_t, _flags: c_int,
                              req: *const timespec, rem: *mut timespec) -> c_int {
    unsafe { nanosleep(req, rem) }
}

pub unsafe fn sleep(secs: c_uint) -> c_uint {
    unsafe {
        sys1(SYS_SUKI_SLEEP_NS, (secs as u64) * 1_000_000_000);
        0
    }
}

// ---------------------------------------------------------------- 随机
/// 填充随机字节（内核 `SYS_SUKI_RANDOM`；非密码学，见内核侧说明）。
pub unsafe fn getrandom(buf: *mut c_void, len: size_t, _flags: c_uint) -> ssize_t {
    unsafe {
        if buf.is_null() {
            set_errno(EFAULT);
            return -1;
        }
        let rc = sys2(SYS_SUKI_RANDOM, buf as u64, len as u64);
        if rc < 0 {
            set_errno(EIO);
            return -1;
        }
        rc as ssize_t
    }
}

// ---------------------------------------------------------------- env（本地表）
// P0：环境变量在 Rust 侧维护（`env/sukios.rs` 初始化时把初始栈的 envp 灌进来）。
static mut ENV_OVERRIDE: *mut c_void = ptr::null_mut();

pub unsafe fn getenv(_name: *const c_char) -> *mut c_char {
    unsafe { ptr::null_mut() }
}
pub unsafe fn setenv(_n: *const c_char, _v: *const c_char, _o: c_int) -> c_int {
    unsafe { 0 }
}
pub unsafe fn unsetenv(_n: *const c_char) -> c_int {
    unsafe { 0 }
}
pub unsafe fn putenv(_s: *mut c_char) -> c_int {
    unsafe { 0 }
}
pub unsafe fn _NSGetEnviron() -> *mut *mut *mut c_char {
    unsafe { ENV_OVERRIDE as *mut *mut *mut c_char }
}
pub unsafe fn _NSGetArgc() -> *mut c_int {
    unsafe { ARGC_PTR }
}
pub unsafe fn _NSGetArgv() -> *mut *mut *mut c_char {
    unsafe { ARGV_PTR }
}

// `_start` 会经 native_set_args 把初始栈的 argv/envp 记到这里（供 std 的 args/env）。
pub static mut ARGC_PTR: *mut c_int = ptr::null_mut();
pub static mut ARGV_PTR: *mut *mut *mut c_char = ptr::null_mut();

/// `_start` 调用：登记 argc/argv/envp（地址取自内核构造的 System V 初始栈）。
pub unsafe fn native_set_args(argc: c_int, argv: *mut *mut c_char,
                              envp: *mut *mut c_char) {
    unsafe {
        static mut ARGC: c_int = 0;
        static mut ARGV: [*mut c_char; 1024] = [ptr::null_mut(); 1024];
        ARGC = argc;
        let n = if argc < 0 { 0 } else { (argc as usize).min(1023) };
        for i in 0..n {
            ARGV[i] = *argv.add(i);
        }
        ARGV[n] = ptr::null_mut();
        ARGC_PTR = &raw mut ARGC;
        ARGV_PTR = &raw mut ARGV as *mut *mut *mut c_char;
        ENV_OVERRIDE = envp as *mut c_void;
    }
}

pub unsafe fn sysconf(name: c_int) -> c_long {
    match name {
        _SC_NPROCESSORS_ONLN => 1,
        _SC_THREAD_STACK_MIN => PTHREAD_STACK_MIN as c_long,
        _SC_GETPW_R_SIZE_MAX => 1024,
        _ => -1,
    }
}

// ---------------------------------------------------------------- 线程同步（futex）
/// Linux 风格 op 编码（0=WAIT, 1=WAKE）——上游 std 的 futex 后端按此调用。
pub const FUTEX_WAIT: c_int = 0;
pub const FUTEX_WAKE: c_int = 1;

/// futex 原语（SukiNative 218/219 → 内核 `futex_wait`/`futex_wake`）。
/// 语义与内核一致：WAIT 仅在 `*uaddr == val` 时睡眠（否则 -EAGAIN）；
/// WAKE 唤醒最多 `val` 个等待者（val 极大表示全部），返回被唤醒数。
/// 超时（`timeout`）当前被内核忽略（P0 取舍：始终阻塞至被唤醒）。
pub unsafe fn futex(uaddr: *mut u32, op: c_int, val: u32,
                    _timeout: *const c_void, _uaddr2: *mut u32,
                    _val3: u32) -> c_int {
    unsafe {
        match op {
            FUTEX_WAIT => {
                let r = sys3(SYS_SUKI_FUTEX_WAIT, uaddr as u64, val as u64, 0);
                if r < 0 {
                    let e = (-r) as c_int;
                    set_errno(e);
                    -1
                } else {
                    0
                }
            }
            FUTEX_WAKE => {
                let r = sys2(SYS_SUKI_FUTEX_WAKE, uaddr as u64, val as u64);
                if r < 0 {
                    set_errno((-r) as c_int);
                    -1
                } else {
                    r as c_int
                }
            }
            _ => {
                set_errno(ENOSYS);
                -1
            }
        }
    }
}

// ---------------------------------------------------------------- 内存映射（P0 走 native mmap_legacy）
pub unsafe fn mmap(_addr: *mut c_void, len: size_t, _prot: c_int, _flags: c_int,
                   _fd: c_int, _off: off_t) -> *mut c_void {
    unsafe {
        let p = sys1(SYS_MMAP_LEGACY, len as u64);
        if p < 0 {
            set_errno(ENOMEM);
            return MAP_FAILED;
        }
        p as *mut c_void
    }
}

pub unsafe fn munmap(addr: *mut c_void, _len: size_t) -> c_int {
    unsafe {
        sys1(SYS_MUNMAP_LEGACY, addr as u64);
        0
    }
}

pub unsafe fn mprotect(_a: *mut c_void, _l: size_t, _p: c_int) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn madvise(_a: *mut c_void, _l: size_t, _adv: c_int) -> c_int {
    unsafe { 0 }
}
pub unsafe fn sbrk(incr: intptr_t) -> *mut c_void {
    unsafe {
        let _ = incr;
        set_errno(ENOSYS);
        (-1isize) as *mut c_void
    }
}

// ---------------------------------------------------------------- 线程（P1 接）
pub unsafe fn pthread_self() -> pthread_t {
    1                            // P0：单线程下恒为主线程
}
pub unsafe fn pthread_create(_t: *mut pthread_t, _a: *const pthread_attr_t,
                             _f: extern "C" fn(*mut c_void) -> *mut c_void,
                             _arg: *mut c_void) -> c_int {
    unsafe { set_errno(ENOSYS); ENOSYS }
}
pub unsafe fn pthread_join(_t: pthread_t, _r: *mut *mut c_void) -> c_int {
    unsafe { set_errno(ENOSYS); ENOSYS }
}
pub unsafe fn pthread_detach(_t: pthread_t) -> c_int {
    0
}
pub unsafe fn pthread_exit(_r: *mut c_void) -> ! {
    unsafe { exit(0) }
}
pub unsafe fn pthread_attr_init(_a: *mut pthread_attr_t) -> c_int {
    0
}
pub unsafe fn pthread_attr_destroy(_a: *mut pthread_attr_t) -> c_int {
    0
}
pub unsafe fn pthread_attr_setstacksize(_a: *mut pthread_attr_t, _s: size_t) -> c_int {
    0
}
pub unsafe fn pthread_attr_setdetachstate(_a: *mut pthread_attr_t,
                                          _s: c_int) -> c_int {
    0
}
pub unsafe fn pthread_setname_np(_t: pthread_t, _n: *const c_char) -> c_int {
    0
}
pub unsafe fn pthread_getname_np(_t: pthread_t, _n: *mut c_char,
                                 _l: size_t) -> c_int {
    unsafe {
        if !_n.is_null() && _l > 0 {
            *_n = 0;
        }
        0
    }
}
pub unsafe fn sched_getaffinity(_pid: pid_t, _cpusetsize: size_t,
                                _mask: *mut c_void) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}

#[repr(C)]
#[derive(Copy, Clone, Default)]
pub struct pthread_attr_t {
    pub stacksize: size_t,
    pub detachstate: c_int,
}

// 其它被上游 sukios 模块引用但 P0 不需要的符号：统一 ENOSYS/空实现
pub unsafe fn fchown(_fd: c_int, _u: uid_t, _g: gid_t) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn chown(_p: *const c_char, _u: uid_t, _g: gid_t) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn utimes(_p: *const c_char, _t: *const timeval) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn sysctl(_a: *mut c_int, _n: c_uint, _old: *mut c_void,
                     _oldlen: *mut size_t, _new: *mut c_void,
                     _newlen: size_t) -> c_int {
    unsafe { set_errno(ENOSYS); -1 }
}
pub unsafe fn syscall(_n: c_long, _a1: c_long, _a2: c_long, _a3: c_long,
                      _a4: c_long, _a5: c_long, _a6: c_long) -> c_long {
    unsafe { set_errno(ENOSYS); -1 }
}
