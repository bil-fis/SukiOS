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
#include <kernel/serial.h>
#include <mm/vmm.h>
#include <mm/vma.h>
#include <mm/kmalloc.h>
#include <ipc/port.h>
#include <kernel/elf.h>
#include <ipc/fs_proto.h>
#include <kernel/hda.h>
#include <kernel/acpi.h>   /* acpi_poweroff：SYS_REBOOT(mode!=0) 经 ACPI S5 软关机 */
#include <kernel/percpu.h>   /* cpu_index()/MAX_CPUS：H8 per-CPU syscall 缓冲 */

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
    task_t *t = sched_current();
    uint64_t cr3 = t->cr3;
    uint64_t base = start & ~((uint64_t)PAGE_SIZE - 1);
    for (uint64_t a = base; a <= end; a += PAGE_SIZE) {
        uint64_t pte = vmm_pte(cr3, a);
        if (!(pte & PTE_PRESENT)) {
            /* P0-5：未映射页可能落在按需 VMA（mmap 匿名区/栈增长区）内——
             * 内核代替用户先行补页（等价于用户自己触发 #PF），成功则该页
             * 立即可用；失败才判 EFAULT。 */
            if (!vma_populate(t, a, write)) {
                return false;
            }
            continue;
        }
        if (write && !(pte & PTE_WRITE)) {
            /* P0-5：只读 PTE 若带 COW 位，代替用户执行写时复制断开 */
            if (!((pte & PTE_COW) && vma_populate(t, a, true))) {
                return false;
            }
        }
    }
    return true;
}

/* L3 修复（SMAP 用户态访问围栏）：g_smap_enabled 由 boot.S 在探测到 CPU
 * 支持 SMAP(CPUID.07 EBX.bit19) 并置位 CR4.SMAP 后置 1。仅当该标志为真时
 * 才在 copy_*_user 内执行 STAC/CLAC——因为 STAC/CLAC 指令本身依赖 SMAP
 * 特性，在不支持的 CPU 上执行会触发 #UD 三重故障。SMAP 开启后，内核以
 * 管理者态访问用户页会被阻塞，故任何触碰用户内存的路径必须先用 STAC 临时
 * 放开（此处是系统唯一的用户指针解引用边界：copy_from_user/copy_to_user，
 * 以及 port.c 经由这两个函数转发的 mach_msg 数据；elf.c 经 PHYS_TO_VIRT
 * 内核直映写用户页，不涉及用户虚拟地址，不受 SMAP 约束）。 */
uint8_t g_smap_enabled = 0;

static inline void smap_stac(void)
{
    if (g_smap_enabled) {
        __asm__ volatile("stac" ::: "cc", "memory");
    }
}
static inline void smap_clac(void)
{
    if (g_smap_enabled) {
        __asm__ volatile("clac" ::: "cc", "memory");
    }
}

size_t copy_from_user(void *dest, const void *user_src, size_t n)
{
    if (!user_access_ok(user_src, n, false)) {
        return 0;                      /* 返回 0 = EFAULT，不触发 panic */
    }
    /* L3：SMAP 开启时，用户页对管理者态不可直接访问，须 STAC 临时放行 */
    smap_stac();
    memcpy(dest, user_src, n);
    smap_clac();
    return n;
}

size_t copy_to_user(void *user_dest, const void *src, size_t n)
{
    if (!user_access_ok(user_dest, n, true)) {
        return 0;
    }
    /* L3：同上，写用户页前 STAC 放行 */
    smap_stac();
    memcpy(user_dest, src, n);
    smap_clac();
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

/* 4: sys_debug_write(buf, len) —— 调试输出，验证 copy_from_user
 * L1 修复：旧实现把单条硬截断到 256B 并静默丢弃剩余字节，长日志（如
 * 内核转储、大段追踪）被截断丢失。改为分块循环：每次从用户态拷入最多
 * 255 字节的临时缓冲并立即 kprintf，直至全部 len 处理完毕；中途遇非法
 * 用户指针则停止并返回已成功写入的字节数（done>0 时不报 -1，保证已输出
 * 部分不丢失）。彻底消除"单条截断导致可观测性缺失"。 */
static uint64_t sys_debug_write(uint64_t buf_uptr, uint64_t len)
{
    char kbuf[256];
    uint64_t done = 0;
    while (done < len) {
        size_t chunk = (size_t)(len - done);
        if (chunk > sizeof(kbuf) - 1) {
            chunk = sizeof(kbuf) - 1;
        }
        if (copy_from_user(kbuf, (const void *)(buf_uptr + done), chunk) != chunk) {
            /* 非法用户指针：返回已输出部分（done>0）或 -1（一条都没输出） */
            return done ? done : (uint64_t)-1;
        }
        kbuf[chunk] = '\0';
        /* 用户态输出走独立的 user_puts() 后端（直接写帧缓冲+串口），
         * 不掺入内核诊断的 g_kernel_fb_diag 开关——即使内核诊断已退出
         * 帧缓冲，shell/UI 文本仍稳定显示。 */
        user_puts(kbuf);
        done += chunk;
    }
    return done;
}

/* 5: sys_input_read —— 非阻塞取原始扫描码；无数据返回 (uint64_t)-1 */
static uint64_t sys_input_read(void)
{
    int sc = keyboard_get_scancode();
    return (sc < 0) ? (uint64_t)-1 : (uint64_t)sc;
}

/* 16: sys_serial_read —— 非阻塞读 COM1 控制台输入；无数据返回 (uint64_t)-1，
 * 否则返回 0..255 的 ASCII 字节。供 Ring3 INPUT_SERVER 把串口作为控制台
 * 输入源（headless QEMU 经 -serial 注入、物理部署经 COM1 控制台）。 */
static uint64_t sys_serial_read(void)
{
    int ch = serial_read();
    return (ch < 0) ? (uint64_t)-1 : (uint64_t)(uint8_t)ch;
}

/* 6: sys_reboot —— mode=0: 经 8042 键盘控制器脉冲 CPU RESET 线重启；
 *    mode!=0: 经 ACPI S5 软关机（见 acpi_poweroff）。二者均不返回。 */
static uint64_t sys_reboot(uint64_t mode)
{
    kprintf("[syscall] reboot requested by pid=%lu (mode=%lu)\n",
            (unsigned long)sched_current()->id, (unsigned long)mode);
    if (mode != 0) {
        acpi_poweroff();   /* 不返回 */
    }
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
    /* argc/envc 必须在任何 goto fail 之前初始化：fail 路径按二者释放已分配
     * 缓冲，早期 goto（如 path 拷贝失败）读未初始化值是未定义行为。 */
    int argc = 0;
    int envc = 0;
    size_t pl = 0;
    for (; pl < EXEC_PATH_MAX - 1; pl++) {
        if (copy_from_user(path + pl, (const void *)(path_uptr + pl), 1) != 1) {
            goto fail;
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

    if (argv_uptr) {
        for (int i = 0; i < EXEC_ARG_MAX; i++) {
            uint64_t a;
            if (copy_from_user(&a, (const void *)(argv_uptr + (uint64_t)i * 8),
                               8) != 8) {
                goto fail;   /* M8：不可 return false——会泄漏已分配字符串 */
            }
            if (a == 0) {
                break;
            }
            argv_k[argc] = (char *)kmalloc(EXEC_STR_MAX);
            if (!argv_k[argc]) {
                goto fail;
            }
            char *p = (char *)a;
            int j = 0;
            for (; j < EXEC_STR_MAX - 1; j++) {
                if (copy_from_user(argv_k[argc] + j, p + j, 1) != 1) {
                    goto fail;
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

    if (envp_uptr) {
        for (int i = 0; i < EXEC_ARG_MAX; i++) {
            uint64_t a;
            if (copy_from_user(&a, (const void *)(envp_uptr + (uint64_t)i * 8),
                               8) != 8) {
                goto fail;   /* M8：不可 return false——会泄漏已分配字符串 */
            }
            if (a == 0) {
                break;
            }
            envp_k[envc] = (char *)kmalloc(EXEC_STR_MAX);
            if (!envp_k[envc]) {
                goto fail;
            }
            char *p = (char *)a;
            int j = 0;
            for (; j < EXEC_STR_MAX - 1; j++) {
                if (copy_from_user(envp_k[envc] + j, p + j, 1) != 1) {
                    goto fail;
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

fail:
    /* M8 修复：任一拷贝/分配失败时，释放已成功分配的 argv/envp 字符串，
     * 避免中途失败路径泄漏内核堆（原实现 return false 后调用方以 argc=0
     * 调 exec_free_args 不会释放这些已分配块）。成功路径由调用方负责释放。 */
    for (int i = 0; i < argc; i++) {
        kfree(argv_k[i]);
    }
    for (int i = 0; i < envc; i++) {
        kfree(envp_k[i]);
    }
    return false;
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
    vma_destroy_all(t);                    /* P0-5：旧映像的 VMA 登记随空间作废 */
    vmm_destroy_address_space(old_cr3);
    t->cr3 = new_as;
    /* P0-R2 KPTI：新地址空间的影子 PML4 需要映射本任务（沿用的）内核栈——
     * 旧空间连同旧影子已销毁，缺此步则 iretq 回 Ring3 后首个中断即三重故障 */
    vmm_kpti_map_kstack(new_as, t->kstack_base, t->kstack_top);
    t->user_rip = res.entry;
    t->user_stack_top = res.stack_top;
    /* P0-5：为新映像登记栈自动增长区（已映射 4 页之下的按需区） */
    {
        uint64_t mapped_lo = stack_top - 4UL * PAGE_SIZE;
        vma_insert(t, mapped_lo - (uint64_t)VMA_STACK_GROW_PAGES * PAGE_SIZE,
                   mapped_lo, PTE_WRITE | PTE_NX, VMA_TYPE_STACK);
    }
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
    /* P0-R1：全部逻辑移入 sched.c task_wait_child（g_sched_lock 保护），
     * 原"仅关本地中断"的实现与其它核上子任务退出路径存在数据竞争。 */
    uint64_t rc = 0;
    if (task_wait_child(child_pid, &rc) < 0) {
        return (uint64_t)-1;
    }
    return rc;
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
    /* H8 修复（P0-R1）：对称多核后 syscall 可在多核并发执行，静态单例
     * 缓冲会被互踩 —— 改为 per-CPU 缓冲（本核 syscall 路径天然串行：
     * 单任务单核运行，中断处理不会重入 syscall）。 */
    static uint8_t kbuf_cpu[MAX_CPUS][8192];
    uint8_t *kbuf = kbuf_cpu[cpu_index()];
    if (len == 0) {
        return 0;
    }
    uint64_t chunk = len < sizeof(kbuf_cpu[0]) ? len : sizeof(kbuf_cpu[0]);
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

/* 14: sys_mmap(len, prot) —— P0-5 匿名按需映射。
 * prot：bit0=可写（映射恒不可执行 NX，W^X 红线：mmap 区绝不给执行权）。
 * 语义：仅登记 VMA，不分配物理页；首次触碰经 #PF -> vma_populate 零页
 * 填充（demand zero-fill）。返回基址，失败返回 0。 */
static uint64_t sys_mmap(uint64_t len, uint64_t prot)
{
    task_t *t = sched_current();
    if (len == 0 || len > VMA_MMAP_MAX) {
        return 0;
    }
    len = (len + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);
    uint64_t base = vma_find_free(t, VMA_MMAP_BASE, VMA_MMAP_TOP, len);
    if (!base) {
        return 0;
    }
    uint64_t vprot = PTE_NX | ((prot & 1) ? PTE_WRITE : 0);
    if (!vma_insert(t, base, base + len, vprot, VMA_TYPE_ANON)) {
        return 0;
    }
    return base;
}

/* 15: sys_munmap(addr, len) —— 解除映射并释放已填充页。0=成功。
 * 限定 mmap 区间内操作，禁止用户 munmap 自己的代码段/栈（那属 execve/exit
 * 的整空间销毁路径）。 */
static uint64_t sys_munmap(uint64_t addr, uint64_t len)
{
    task_t *t = sched_current();
    if (len == 0 || (addr & (PAGE_SIZE - 1)) ||
        addr < VMA_MMAP_BASE || addr + len > VMA_MMAP_TOP ||
        addr + len < addr) {
        return (uint64_t)-1;
    }
    len = (len + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);
    return vma_unmap_range(t, addr, addr + len) ? 0 : (uint64_t)-1;
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
    case SYS_REBOOT:      return sys_reboot(a1);
    case SYS_PORT_CLAIM:  return sys_port_claim(a1);
    case SYS_EXECVE:      return sys_execve(a1, a2, a3);
    case SYS_WAIT:        return sys_wait(a1);
    case SYS_AUDIO_OPEN:  return sys_audio_open(a1, a2, a3);
    case SYS_AUDIO_WRITE: return sys_audio_write(a1, a2);
    case SYS_AUDIO_QUEUED:return sys_audio_queued();
    case SYS_AUDIO_STOP:  return sys_audio_stop();
    case SYS_MMAP:        return sys_mmap(a1, a2);
    case SYS_MUNMAP:      return sys_munmap(a1, a2);
    case SYS_SERIAL_READ: return sys_serial_read();
    default:
        kprintf("[syscall] unknown syscall %lu from pid=%lu (user_rip=%p)\n",
                (unsigned long)num, (unsigned long)sched_current()->id,
                (void *)sched_current()->scr_rip);
        return (uint64_t)-1;
    }
}
