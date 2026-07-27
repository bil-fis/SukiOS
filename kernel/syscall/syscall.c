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
#include <mm/kmalloc.h>
#include <ipc/port.h>
#include <kernel/elf.h>
#include <ipc/fs_proto.h>
#include <kernel/hda.h>

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

/* 保存/恢复 IF 的临界区原语（嵌套安全）：与无条件 sti 不同，
 * 仅当进入时 IF=1 才在 restore 时重新开中断。单核使用 cli/sti。 */
static inline uint64_t irq_save(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void irq_restore(uint64_t flags)
{
    if (flags & (1UL << 9)) {
        __asm__ volatile("sti" ::: "memory");
    }
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

/* 2: sys_task_exit */
static uint64_t sys_task_exit(uint64_t code)
{
    kprintf("[syscall] task '%s' pid=%lu exit(code=%lu)\n",
            sched_current()->name,
            (unsigned long)sched_current()->id,
            (unsigned long)code);
    task_exit_current(code);           /* 不返回 */
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

/* 8: sys_execve / 1: sys_task_spawn —— ELF 加载（替换自身 或 新建子任务） */
#define EXEC_PATH_MAX   256
#define EXEC_ARG_MAX    ELF_ARG_MAX   /* 与 elf_build_stack 容量共享同一常量（H1） */
#define EXEC_STR_MAX    512
#define EXEC_ELF_MAX    (16 * 4096)   /* 与 OOL 单条 16 页上限一致 */

/* 这两个 scratch 由 syscall_entry.S 存入“当前任务”的 scr_rip/scr_rsp
 * （task_t 字段，经 g_scratch 指针访问）；execve 改写它们使 syscall 返回
 * 帧跳转到新程序入口而非原程序断点。这里直接用当前任务结构体字段。 */

/* 轻量熵（execve 用户栈 ASLR） */
static uint64_t execve_rand(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t x = ((uint64_t)hi << 32) | lo;
    x ^= x >> 33; x *= 0xFF51AFD7ED558CCDUL; x ^= x >> 33;
    return x;
}

/* 从用户态拷贝路径 + argv[]/envp[] 指针数组及字符串到内核缓冲。
 * 成功返回 true，并把解析出的 argc/envc、内核 argv/envp 指针数组填入
 * 调用方提供的数组；失败时返回 false（调用方负责释放已分配缓冲）。 */
static bool exec_copy_args(uint64_t path_uptr, uint64_t argv_uptr,
                           uint64_t envp_uptr, char *path, size_t *pl_out,
                           char *argv_k[EXEC_ARG_MAX], int *argc_out,
                           char *envp_k[EXEC_ARG_MAX], int *envc_out)
{
    size_t pl = 0;
    for (; pl < EXEC_PATH_MAX - 1; pl++) {
        if (copy_from_user(path + pl, (const void *)(path_uptr + pl), 1) != 1) {
            return false;
        }
        if (path[pl] == '\0') {
            break;
        }
    }
    path[pl] = '\0';
    if (pl == 0) {
        return false;
    }
    *pl_out = pl;

    int argc = 0;
    if (argv_uptr) {
        for (int i = 0; i < EXEC_ARG_MAX; i++) {
            uint64_t a;
            if (copy_from_user(&a, (const void *)(argv_uptr + (uint64_t)i * 8),
                               8) != 8) {
                return false;
            }
            if (a == 0) {
                break;
            }
            argv_k[argc] = (char *)kmalloc(EXEC_STR_MAX);
            if (!argv_k[argc]) {
                return false;
            }
            char *p = (char *)a;
            int j = 0;
            for (; j < EXEC_STR_MAX - 1; j++) {
                if (copy_from_user(argv_k[argc] + j, p + j, 1) != 1) {
                    return false;
                }
                if (argv_k[argc][j] == '\0') {
                    break;
                }
            }
            argv_k[argc][j] = '\0';
            argc++;
        }
    }
    *argc_out = argc;

    int envc = 0;
    if (envp_uptr) {
        for (int i = 0; i < EXEC_ARG_MAX; i++) {
            uint64_t a;
            if (copy_from_user(&a, (const void *)(envp_uptr + (uint64_t)i * 8),
                               8) != 8) {
                return false;
            }
            if (a == 0) {
                break;
            }
            envp_k[envc] = (char *)kmalloc(EXEC_STR_MAX);
            if (!envp_k[envc]) {
                return false;
            }
            char *p = (char *)a;
            int j = 0;
            for (; j < EXEC_STR_MAX - 1; j++) {
                if (copy_from_user(envp_k[envc] + j, p + j, 1) != 1) {
                    return false;
                }
                if (envp_k[envc][j] == '\0') {
                    break;
                }
            }
            envp_k[envc][j] = '\0';
            envc++;
        }
    }
    *envc_out = envc;
    return true;
}

static void exec_free_args(char *argv_k[EXEC_ARG_MAX], int argc,
                           char *envp_k[EXEC_ARG_MAX], int envc)
{
    for (int i = 0; i < argc; i++) {
        kfree(argv_k[i]);
    }
    for (int i = 0; i < envc; i++) {
        kfree(envp_k[i]);
    }
}

/* 内核作为 IPC 客户端，从 FS_SERVER 读取 path 指向的文件的完整内容。
 * 成功返回 kmalloc 的缓冲（首部为 fs_resp_t，其后为文件字节）；
 * 通过 elf_data / elf_len 给出 ELF 数据区间。失败返回 NULL（并释放资源）。
 * 调用方负责 kfree 返回的缓冲。 */
static uint8_t *exec_read_file(const char *path, size_t pl,
                               const uint8_t **elf_data, size_t *elf_len)
{
    uint32_t rp = port_allocate(sched_current());
    if (!rp) {
        return NULL;
    }
    uint8_t *elfbuf = (uint8_t *)kmalloc(EXEC_ELF_MAX);
    if (!elfbuf) {
        port_free(rp);
        return NULL;
    }

    uint8_t req[sizeof(mach_msg_header_t) + EXEC_PATH_MAX];
    memset(req, 0, sizeof(req));
    mach_msg_header_t *rh = (mach_msg_header_t *)req;
    rh->msgh_bits = 0;
    rh->msgh_size = sizeof(*rh) + (uint32_t)pl + 1;
    rh->msgh_remote_port = FS_PORT;
    rh->msgh_local_port = rp;
    rh->msgh_id = FS_MSG_READ_FILE;
    rh->msgh_reserved = 0;
    memcpy(req + sizeof(*rh), path, pl + 1);

    if (ipc_send_kernel(FS_PORT, req, rh->msgh_size) != MACH_MSG_SUCCESS) {
        port_free(rp);
        kfree(elfbuf);
        return NULL;
    }

    uint8_t inline_buf[sizeof(mach_msg_header_t) + sizeof(fs_resp_t)];
    uint32_t inline_out = 0, elf_out = 0;
    if (ipc_recv_ool_kernel(rp, inline_buf, sizeof(inline_buf), &inline_out,
                            elfbuf, EXEC_ELF_MAX, &elf_out, true)
            != MACH_MSG_SUCCESS) {
        port_free(rp);
        kfree(elfbuf);
        return NULL;
    }
    port_free(rp);

    /* OOL 数据首部即 fs_resp（status/length），其后为 ELF 字节 */
    fs_resp_t *fr = (fs_resp_t *)elfbuf;
    if (fr->status != FS_OK || elf_out <= sizeof(fs_resp_t)) {
        kfree(elfbuf);
        return NULL;
    }
    *elf_data = elfbuf + sizeof(fs_resp_t);
    *elf_len = fr->length;
    return elfbuf;     /* 调用方负责 kfree */
}

/* 8: sys_execve —— 加载并执行 ELF 映像，替换当前进程（完整功能，不简化） */
static uint64_t sys_execve(uint64_t path_uptr, uint64_t argv_uptr,
                           uint64_t envp_uptr)
{
    char path[EXEC_PATH_MAX];
    char *argv_k[EXEC_ARG_MAX] = {0};
    char *envp_k[EXEC_ARG_MAX] = {0};
    size_t pl = 0; int argc = 0, envc = 0;

    if (!exec_copy_args(path_uptr, argv_uptr, envp_uptr, path, &pl,
                        argv_k, &argc, envp_k, &envc)) {
        exec_free_args(argv_k, argc, envp_k, envc);
        return (uint64_t)-1;
    }

    const uint8_t *elf_data = NULL;
    size_t elf_len = 0;
    uint8_t *elfbuf = exec_read_file(path, pl, &elf_data, &elf_len);
    if (!elfbuf) {
        exec_free_args(argv_k, argc, envp_k, envc);
        return (uint64_t)-1;
    }

    /* 加载并替换当前进程映像 */
    task_t *t = sched_current();
    port_reap_ool(t);                       /* 释放旧进程持有的 OOL 映射 */
    uint64_t old_cr3 = t->cr3;
    uint64_t new_as = vmm_create_address_space();
    if (!new_as) {
        kfree(elfbuf);
        exec_free_args(argv_k, argc, envp_k, envc);
        return (uint64_t)-1;
    }
    elf_load_result_t res;
    uint64_t stack_top = 0x00007FFFFFFFF000UL
                         - (execve_rand() & 0xFF) * PAGE_SIZE;
    if (!elf_load(new_as, elf_data, elf_len, argc,
                  (const char *const *)argv_k, envc,
                  (const char *const *)envp_k, stack_top, 4, &res)) {
        vmm_destroy_address_space(new_as);
        kfree(elfbuf);
        exec_free_args(argv_k, argc, envp_k, envc);
        return (uint64_t)-1;
    }

    /* 切换地址空间：先切内核，再销毁旧用户空间，避免悬空 CR3 */
    vmm_switch(vmm_kernel_pml4());
    vmm_destroy_address_space(old_cr3);
    t->cr3 = new_as;
    t->user_rip = res.entry;
    t->user_stack_top = res.stack_top;
    /* 任务名替换为新映像名（取路径最后一段），便于日志辨识 */
    {
        const char *bn = path;
        for (size_t k = 0; k < pl; k++) {
            if (path[k] == '/') {
                bn = path + k + 1;
            }
        }
        size_t nl = 0;
        while (bn[nl] && nl < sizeof(t->name) - 1) {
            t->name[nl] = bn[nl];
            nl++;
        }
        t->name[nl] = '\0';
    }

    /* 让 syscall 返回帧跳转到新程序（改写当前任务的返回暂存） */
    t->scr_rip = res.entry;
    t->scr_rsp = res.stack_top;
    vmm_switch(new_as);                    /* iretq 后用户态用新地址空间 */

    kfree(elfbuf);
    exec_free_args(argv_k, argc, envp_k, envc);
    return 0;                              /* 返回用户态新程序 */
}

/* 1: sys_task_spawn —— 新建 Ring3 子任务并装载 ELF（不替换当前进程）。
 * 类 Unix fork+exec：内核读取 ELF、建立独立地址空间、插入就绪环，并
 * 记录父 PID；返回子任务 PID。父任务应随后 sys_wait(pid) 等待其结束。 */
static uint64_t sys_task_spawn(uint64_t path_uptr, uint64_t argv_uptr,
                               uint64_t envp_uptr)
{
    char path[EXEC_PATH_MAX];
    char *argv_k[EXEC_ARG_MAX] = {0};
    char *envp_k[EXEC_ARG_MAX] = {0};
    size_t pl = 0; int argc = 0, envc = 0;

    if (!exec_copy_args(path_uptr, argv_uptr, envp_uptr, path, &pl,
                        argv_k, &argc, envp_k, &envc)) {
        exec_free_args(argv_k, argc, envp_k, envc);
        return (uint64_t)-1;
    }

    const uint8_t *elf_data = NULL;
    size_t elf_len = 0;
    uint8_t *elfbuf = exec_read_file(path, pl, &elf_data, &elf_len);
    if (!elfbuf) {
        exec_free_args(argv_k, argc, envp_k, envc);
        return (uint64_t)-1;
    }

    /* 取映像 basename 作任务名 */
    const char *bn = path;
    for (size_t k = 0; k < pl; k++) {
        if (path[k] == '/') {
            bn = path + k + 1;
        }
    }

    task_t *child = task_create_user_args(elf_data, elf_len, argc,
                                          (const char *const *)argv_k, envc,
                                          (const char *const *)envp_k, bn);
    kfree(elfbuf);
    exec_free_args(argv_k, argc, envp_k, envc);
    if (!child) {
        return (uint64_t)-1;
    }

    /* 记录父子关系（用 PID，避免父退出后悬空指针） */
    child->parent_id = sched_current()->id;

    kprintf("[syscall] spawn: parent pid=%lu -> child '%s' pid=%lu\n",
            (unsigned long)sched_current()->id, child->name,
            (unsigned long)child->id);
    return child->id;
}

/* 9: sys_wait —— 阻塞等待子任务 pid 退出，返回其退出码。
 * pid 非本任务子进程（或不存在）时立即返回 -1。子任务已先退出（zombie）
 * 时直接返回其退出码，随后该 zombie 被回收（仅此一次）。 */
static uint64_t sys_wait(uint64_t child_pid)
{
    task_t *cur = sched_current();
    uint64_t f = irq_save();
    task_t *child = task_lookup(child_pid);
    if (!child || child->parent_id != cur->id) {
        irq_restore(f);
        return (uint64_t)-1;
    }
    if (child->zombie) {
        uint64_t rc = child->exit_code;
        task_reap(child);            /* 立即回收：二次 wait 将 lookup 失败返回 -1 */
        irq_restore(f);
        return rc;
    }
    /* 阻塞：登记到子任务的等待者链表，关中断下切换；子退出时唤醒本任务
     * 并写入退出码。注意：schedule() 须在中断关闭下调用（与 port.c 一致）。 */
    cur->state     = BLOCKED;
    cur->wait_link = child->waiters;
    child->waiters = cur;
    schedule();                      /* 关中断下切换；被唤醒后继续 */
    irq_restore(f);
    /* 被唤醒：退出码已由子任务 task_exit_current 写入 cur->wait_result */
    return cur->wait_result;
}

/* 10: sys_audio_open(rate, channels, bits) —— 打开 HDA 输出流 */
static uint64_t sys_audio_open(uint64_t rate, uint64_t channels, uint64_t bits)
{
    return (uint64_t)hda_pcm_open((uint32_t)rate, (uint32_t)channels,
                                  (uint32_t)bits);
}

/* 11: sys_audio_write(buf, len) —— 把用户 PCM 拷入内核播放环。
 * 红线：用户指针一律经 copy_from_user 拷到内核缓冲后再交给驱动。
 * 分块拷贝，单次上限 = 内核暂存缓冲；返回实际写入驱动环的字节数。 */
static uint64_t sys_audio_write(uint64_t buf_uptr, uint64_t len)
{
    static uint8_t kbuf[8192];      /* syscall 串行执行（无并发），静态即可 */
    if (len == 0) {
        return 0;
    }
    uint64_t chunk = len < sizeof(kbuf) ? len : sizeof(kbuf);
    size_t got = copy_from_user(kbuf, (const void *)buf_uptr, (size_t)chunk);
    if (got == 0) {
        return (uint64_t)-1;        /* 非法用户指针 */
    }
    int64_t wr = hda_pcm_write(kbuf, got);
    if (wr < 0) {
        return (uint64_t)-1;
    }
    return (uint64_t)wr;
}

/* 12: sys_audio_queued —— 环中未播放字节数 */
static uint64_t sys_audio_queued(void)
{
    return hda_pcm_queued();
}

/* 13: sys_audio_stop —— 停止并复位输出流 */
static uint64_t sys_audio_stop(void)
{
    hda_pcm_stop();
    return 0;
}

uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5)
{
    switch (num) {
    case SYS_MACH_MSG:    return sys_mach_msg(a1, a2, a3, a4, a5);
    case SYS_TASK_SPAWN:  return sys_task_spawn(a1, a2, a3);
    case SYS_TASK_EXIT:   return sys_task_exit(a1);
    case SYS_YIELD:       return sys_yield();
    case SYS_DEBUG_WRITE: return sys_debug_write(a1, a2);
    case SYS_INPUT_READ:  return sys_input_read();
    case SYS_REBOOT:      return sys_reboot();
    case SYS_PORT_CLAIM:  return sys_port_claim(a1);
    case SYS_EXECVE:      return sys_execve(a1, a2, a3);
    case SYS_WAIT:        return sys_wait(a1);
    case SYS_AUDIO_OPEN:  return sys_audio_open(a1, a2, a3);
    case SYS_AUDIO_WRITE: return sys_audio_write(a1, a2);
    case SYS_AUDIO_QUEUED:return sys_audio_queued();
    case SYS_AUDIO_STOP:  return sys_audio_stop();
    default:
        kprintf("[syscall] unknown syscall %lu from pid=%lu (user_rip=%p)\n",
                (unsigned long)num, (unsigned long)sched_current()->id,
                (void *)sched_current()->scr_rip);
        return (uint64_t)-1;
    }
}
