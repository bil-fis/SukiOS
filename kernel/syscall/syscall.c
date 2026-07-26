/*
 * kernel/syscall/syscall.c
 * -----------------------------------------------------------------------------
 * 系统调用表与 C 分发器 + copy_from_user/copy_to_user。
 *
 * 红线（手册 9.1）：syscall 处理中严禁直接解引用用户指针，一律先经
 * copy_from_user 拷入内核缓冲区。
 *
 * 调用关系：syscall_entry(汇编) -> syscall_dispatch() -> sys_* 各实现。
 * sys_mach_msg 在阶段七由 IPC 模块提供（弱符号占位）。
 */
#include <kernel/syscall.h>
#include <kernel/task.h>
#include <kernel/console.h>
#include <kernel/string.h>
#include <kernel/keyboard.h>
#include <kernel/io.h>
#include <mm/vmm.h>
#include <ipc/port.h>

/* ---- 用户指针校验（A1 项）----
 * 合法用户区间：[0, USER_SPACE_TOP]，且 [ptr, ptr+n) 不得回绕/越界；
 * 逐页检查页表项存在（copy_from_user）或被映射为可写（copy_to_user），
 * 避免“用户传未映射/只读指针 → 内核缺页 → panic 整机”的致命缺陷。 */
static bool user_access_ok(const void *uptr, size_t n, bool write)
{
    uint64_t start = (uint64_t)uptr;
    if (n == 0) {
        return true;
    }
    uint64_t end = start + n - 1;
    if (end < start) {                 /* 回绕 */
        return false;
    }
    if (end > USER_SPACE_TOP) {
        return false;
    }
    uint64_t cr3 = sched_current()->cr3;
    uint64_t base = start & ~((uint64_t)PAGE_SIZE - 1);
    for (uint64_t a = base; a <= end; a += PAGE_SIZE) {
        uint64_t pte = vmm_pte(cr3, a);
        if (!(pte & PTE_PRESENT)) {
            return false;
        }
        if (write && !(pte & PTE_WRITE)) {
            return false;
        }
    }
    return true;
}

size_t copy_from_user(void *dest, const void *user_src, size_t n)
{
    if (!user_access_ok(user_src, n, false)) {
        return 0;                      /* 返回 0 = EFAULT，不触发 panic */
    }
    memcpy(dest, user_src, n);
    return n;
}

size_t copy_to_user(void *user_dest, const void *src, size_t n)
{
    if (!user_access_ok(user_dest, n, true)) {
        return 0;
    }
    memcpy(user_dest, src, n);
    return n;
}

/* ---- 系统调用实现 ---- */

/* 0: sys_mach_msg —— 阶段七由 kernel/ipc/ 提供强符号实现 */
__attribute__((weak))
uint64_t sys_mach_msg(uint64_t msg_uptr, uint64_t option,
                      uint64_t send_size, uint64_t recv_limit, uint64_t port)
{
    (void)msg_uptr; (void)option; (void)send_size;
    (void)recv_limit; (void)port;
    return (uint64_t)-1;    /* 未实现 */
}

/* 1: sys_task_create —— 最小实现：暂不允许用户态直接建任务 */
static uint64_t sys_task_create(uint64_t a1, uint64_t a2)
{
    (void)a1; (void)a2;
    return (uint64_t)-1;
}

/* 2: sys_task_exit */
static uint64_t sys_task_exit(uint64_t code)
{
    kprintf("[syscall] task '%s' pid=%lu exit(code=%lu)\n",
            sched_current()->name,
            (unsigned long)sched_current()->id,
            (unsigned long)code);
    task_exit_current();               /* 不返回 */
}

/* 3: sys_yield */
static uint64_t sys_yield(void)
{
    task_yield();
    return 0;
}

/* 4: sys_debug_write(buf, len) —— 调试输出，验证 copy_from_user */
static uint64_t sys_debug_write(uint64_t buf_uptr, uint64_t len)
{
    char kbuf[256];
    if (len > sizeof(kbuf) - 1) {
        len = sizeof(kbuf) - 1;
    }
    size_t got = copy_from_user(kbuf, (const void *)buf_uptr, len);
    if (got == 0 && len != 0) {
        return (uint64_t)-1;           /* 非法用户指针 */
    }
    kbuf[got] = '\0';
    kprintf("%s", kbuf);
    return got;
}

/* 5: sys_input_read —— 非阻塞取原始扫描码；无数据返回 (uint64_t)-1 */
static uint64_t sys_input_read(void)
{
    int sc = keyboard_get_scancode();
    return (sc < 0) ? (uint64_t)-1 : (uint64_t)sc;
}

/* 6: sys_reboot —— 通过 8042 键盘控制器脉冲 CPU RESET 线 */
static uint64_t sys_reboot(void)
{
    kprintf("[syscall] reboot requested by pid=%lu\n",
            (unsigned long)sched_current()->id);
    /* 等待 8042 输入缓冲空，然后发送 0xFE (pulse reset) */
    for (int i = 0; i < 100000; i++) {
        if (!(inb(0x64) & 0x02)) {
            break;
        }
    }
    outb(0x64, 0xFE);
    for (;;) {
        __asm__ volatile("hlt");
    }
    return 0;   /* 不可达 */
}

/* 7: sys_port_claim —— 用户态认领端口的接收权（A2 项：IPC 能力） */
static uint64_t sys_port_claim(uint64_t port)
{
    return port_claim((uint32_t)port);
}

uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5)
{
    switch (num) {
    case SYS_MACH_MSG:    return sys_mach_msg(a1, a2, a3, a4, a5);
    case SYS_TASK_CREATE: return sys_task_create(a1, a2);
    case SYS_TASK_EXIT:   return sys_task_exit(a1);
    case SYS_YIELD:       return sys_yield();
    case SYS_DEBUG_WRITE: return sys_debug_write(a1, a2);
    case SYS_INPUT_READ:  return sys_input_read();
    case SYS_REBOOT:      return sys_reboot();
    case SYS_PORT_CLAIM:  return sys_port_claim(a1);
    default:
        kprintf("[syscall] unknown syscall %lu from pid=%lu\n",
                (unsigned long)num, (unsigned long)sched_current()->id);
        return (uint64_t)-1;
    }
}
