/*
 * kernel/console.c
 * -----------------------------------------------------------------------------
 * 统一内核控制台与最小 kprintf 实现。
 *
 * 输出策略：每个字符先写 COM1 串口（永远可用，用于日志），再写主显示：
 *   - 若帧缓冲可用 -> 图形控制台 fbcon
 *   - 否则          -> VGA 文本模式 vga
 *
 * 调用关系：全内核 -> kprintf()/panic() -> serial_write + fbcon/vga。
 *
 * 重要：内核诊断日志与用户态 shell 输出共用帧缓冲会导致按键时屏幕被
 * [ipc]/[sched] 等运行期调试日志刷屏。为此引入 g_kernel_fb_diag 开关：
 *   - 启动阶段（kmain 早期）保持 true，内核初始化日志同时显示到帧缓冲
 *     （用户可见的漂亮启动画面）；
 *   - 进入用户态服务前由 kmain 调 console_set_fb_diag(false) 关闭，
 *     此后内核运行期诊断【只走串口】，帧缓冲专供 Ring3 shell/UI，
 *     按键不再污染图形终端。用户态输出经 sys_debug_write -> user_puts()
 *     独立路径写帧缓冲，完全不经过本开关。
 */
#include <kernel/console.h>
#include <kernel/serial.h>
#include <kernel/framebuffer.h>
#include <kernel/vga_text.h>
#include <kernel/ksym.h>          /* EXPORT_SYMBOL 内核符号导出 */
EXPORT_SYMBOL(kprintf);          /* 供 .kdr 内核模块调用 */
#include <kernel/diagnostics.h>
#include <kernel/spinlock.h>
#include <kernel/smp.h>
#include <kernel/display_cfg.h>
#include <ipc/port.h>
#include <stdarg.h>

/* P0-3：kprintf 跨 CPU 串行化自旋锁。spin_lock_irqsave = 关本地中断 +
 * ticket 锁，单核下锁必然立即成功（退化为原 cli/sti 行为，零额外开销），
 * SMP 下保证一条消息跨 CPU 原子输出（BSP 日志 vs AP 启动日志不撕裂）。 */
static spinlock_t g_kp_lock = SPINLOCK_INIT("kprintf");

static bool g_use_fb = false;
/* 内核诊断是否镜像到帧缓冲（默认开；进入用户态服务前由 kmain 关闭）。 */
static bool g_kernel_fb_diag = true;
/* 启动期(第三步系统初始化)是否把日志镜像到帧缓冲：仅 -v/--verbose 时为真。
 * 由 kmain 解析 GRUB 命令行设置；串口恒定输出。 */
bool g_boot_verbose = false;
/* M18 修复：kprintf 重入深度计数。单核下 irq_save 已保证一条消息原子输出，
 * 但若在输出途中触发 #PF 等异常二次进入 kprintf（如 panic 路径），会递归
 * 打印导致栈耗尽。超过阈值即放弃本次输出，既保证普通场景正常又防致命递归。
 * SMP 后的跨 CPU 串行化自旋锁随 P0-3 引入。 */
static volatile int g_kp_depth = 0;

/* ---- 早期控制台环形管道（详见 console.h 注释）---- */
static char     g_console_pipe[CONSOLE_PIPE_SIZE];
static volatile size_t g_pipe_head = 0;   /* 生产者写指针（环形索引） */
static volatile size_t g_pipe_tail = 0;   /* 消费者读指针（环形索引） */
static spinlock_t g_pipe_lock = SPINLOCK_INIT("conpipe");

void console_init(void)
{
    g_use_fb = fb_available();
    if (!g_use_fb) {
        vga_init();
    }
}

/* 单条 kprintf 已输出字符计数（kprintf 持锁串行化，单核/多核均安全）。
 * kputc 递增它，kprintf 据此对超长消息截断，防止 fmt/%s/宽度损坏导致
 * 的无限输出冻结系统。 */
static volatile size_t g_kprintf_nout = 0;

void kputc(char c)
{
    g_kprintf_nout++;
    serial_write(c);   /* 串口恒定输出，便于无图形场景也可见内核日志 */

    /*
     * 显示服务接管帧缓冲后（g_display_active），内核诊断【不再直接写屏】——
     * 否则会覆盖显示服务合成的桌面。此时把本要送 fbcon 的字符「捕获」进
     * 早期控制台环形管道，供后续用户态 shell 经 SYS_CONSOLE_READ 读回
     * （类似 dmesg）。串口仍照常输出，因此无图形调试不受影响。
     *
     * 关系式：
     *   - 显示未激活（启动早期/纯文本回退）：照旧写 fbcon/vga。
     *   - 显示已激活：仅捕获进管道 + 串口；除非 panic() 强制把
     *     g_kernel_fb_diag 拉回 true（致命错误须图形终端可见）。
     */
    if (g_display_active) {
        if (g_kernel_fb_diag) {           /* panic 等致命场景：直接屏显 */
            if (g_use_fb) fbcon_putc(c);
            else          vga_putc(c);
        }
        /* 捕获进环形管道（独立于 fbcon，供 SYS_CONSOLE_READ 读取） */
        uint64_t pl = spin_lock_irqsave(&g_pipe_lock);
        size_t next = (g_pipe_head + 1) % CONSOLE_PIPE_SIZE;
        if (next != g_pipe_tail) {        /* 未满：写入 */
            g_console_pipe[g_pipe_head] = c;
            g_pipe_head = next;
        } else {
            /* 环形满：丢弃最旧字符（覆盖式），保证最新日志不丢 */
            g_pipe_tail = (g_pipe_tail + 1) % CONSOLE_PIPE_SIZE;
            g_console_pipe[g_pipe_head] = c;
            g_pipe_head = next;
        }
        spin_unlock_irqrestore(&g_pipe_lock, pl);
        return;
    }

    /* 显示未激活：正常路径。
     * 启动期(第三步系统初始化)默认仅串口输出；仅当 -v/--verbose（g_boot_verbose）
     * 时才镜像到帧缓冲。显示服务接管后由上方 g_display_active 分支处理（捕获进管道）。 */
    if (g_use_fb) {
        if (g_boot_verbose)
            fbcon_putc(c);
        /* 否则仅串口（不写 VGA，避免无谓寄存器访问） */
    } else {
        vga_putc(c);
    }
}

/* SYS_CONSOLE_READ 处理体：把环形管道中累积的字符拷贝到用户态缓冲。
 * 返回实际拷贝字节数（0 表示暂无数据）。user_ptr 由调用方 copy_to_user 校验。 */
size_t console_pipe_read(char *dst, size_t max)
{
    if (max == 0) return 0;
    uint64_t pl = spin_lock_irqsave(&g_pipe_lock);
    size_t avail = (g_pipe_head >= g_pipe_tail)
                       ? (g_pipe_head - g_pipe_tail)
                       : (CONSOLE_PIPE_SIZE - g_pipe_tail + g_pipe_head);
    size_t n = (avail < max) ? avail : max;
    for (size_t i = 0; i < n; i++) {
        dst[i] = g_console_pipe[g_pipe_tail];
        g_pipe_tail = (g_pipe_tail + 1) % CONSOLE_PIPE_SIZE;
    }
    spin_unlock_irqrestore(&g_pipe_lock, pl);
    return n;
}

size_t console_pipe_avail(void)
{
    uint64_t pl = spin_lock_irqsave(&g_pipe_lock);
    size_t avail = (g_pipe_head >= g_pipe_tail)
                       ? (g_pipe_head - g_pipe_tail)
                       : (CONSOLE_PIPE_SIZE - g_pipe_tail + g_pipe_head);
    spin_unlock_irqrestore(&g_pipe_lock, pl);
    return avail;
}

/* ========================================================================== */
/*  用户 TTY 环形管道（user TTY ring pipe）                                    */
/* ========================================================================== */
/* 与内核日志管道（g_console_pipe）相互独立：本管道只装「Ring3 程序经 fd 1/2
 * 写 TTY」的输出，供 shell 作为终端渲染到自己的窗口；不混入内核诊断日志。 */
static char     g_user_tty_pipe[USER_TTY_PIPE_SIZE];
static volatile size_t g_utty_head = 0;
static volatile size_t g_utty_tail = 0;
static spinlock_t g_utty_lock = SPINLOCK_INIT("uttypipe");

/* 生产者：tty_write() -> user_tty_out() 调用。写满覆盖最旧字符，绝不阻塞。 */
static void user_tty_pipe_write(const char *s)
{
    uint64_t pl = spin_lock_irqsave(&g_utty_lock);
    while (*s) {
        size_t next = (g_utty_head + 1) % USER_TTY_PIPE_SIZE;
        if (next != g_utty_tail) {
            g_user_tty_pipe[g_utty_head] = *s++;
            g_utty_head = next;
        } else {
            /* 环形满：丢弃最旧字符（覆盖式），保证最新文本不丢 */
            g_utty_tail = (g_utty_tail + 1) % USER_TTY_PIPE_SIZE;
            g_user_tty_pipe[g_utty_head] = *s++;
            g_utty_head = next;
        }
    }
    spin_unlock_irqrestore(&g_utty_lock, pl);
}

size_t user_tty_pipe_read(char *dst, size_t max)
{
    if (max == 0) return 0;
    uint64_t pl = spin_lock_irqsave(&g_utty_lock);
    size_t avail = (g_utty_head >= g_utty_tail)
                       ? (g_utty_head - g_utty_tail)
                       : (USER_TTY_PIPE_SIZE - g_utty_tail + g_utty_head);
    size_t n = (avail < max) ? avail : max;
    for (size_t i = 0; i < n; i++) {
        dst[i] = g_user_tty_pipe[g_utty_tail];
        g_utty_tail = (g_utty_tail + 1) % USER_TTY_PIPE_SIZE;
    }
    spin_unlock_irqrestore(&g_utty_lock, pl);
    return n;
}

size_t user_tty_pipe_avail(void)
{
    uint64_t pl = spin_lock_irqsave(&g_utty_lock);
    size_t avail = (g_utty_head >= g_utty_tail)
                       ? (g_utty_head - g_utty_tail)
                       : (USER_TTY_PIPE_SIZE - g_utty_tail + g_utty_head);
    spin_unlock_irqrestore(&g_utty_lock, pl);
    return avail;
}

/* 受限字符串输出：防御 fmt 指向无 NULL 终止的损坏内存时陷入无限循环
 * （会永久持有 g_kp_lock / 端口锁导致系统冻结）。超限即截断并告警。 */
void kputs(const char *s)
{
    size_t n = 0;
    while (*s) {
        kputc(*s++);
        if (++n >= 2048) {
            /* 字符串异常长：极可能是被踩坏的指针。绕过 kprintf 锁直接告警。 */
            serial_writestr("\n[console] kputs OVERFLOW (bad %s pointer?) ptr=");
            serial_write_hex((uint64_t)(uintptr_t)s);
            serial_writestr("\n");
            break;
        }
    }
}

/* --- 数字格式化辅助 --- */
static void print_uint(uint64_t val, unsigned base, bool upper, int min_width, char pad)
{
    char buf[65];
    const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;
    if (val == 0) {
        buf[i++] = '0';
    }
    while (val > 0) {
        buf[i++] = digs[val % base];
        val /= base;
    }
    /* 防御：min_width 必须为合理小值（格式串损坏时可能解析出巨大值，
     * 导致 below 的填充循环输出数百万字符使系统看似冻结）。截断到 64。 */
    if (min_width > 64) {
        min_width = 64;
    }
    while (i < min_width) {
        buf[i++] = pad;
    }
    while (i > 0) {
        kputc(buf[--i]);
    }
}

static void print_int(int64_t val)
{
    if (val < 0) {
        kputc('-');
        print_uint((uint64_t)(-val), 10, false, 0, ' ');
    } else {
        print_uint((uint64_t)val, 10, false, 0, ' ');
    }
}

/* 控制台文本消息：内核经 DISPLAY_PORT 转发给显示服务，由其渲染到桌面终端窗口。
 * 与 user/display_server.c 的 DISP_MSG_TEXT 保持一致（用 #ifndef 防止重复定义）。 */
#ifndef DISP_MSG_TEXT
#define DISP_MSG_TEXT 1
#endif
#define USER_PUTS_MAX 256   /* 单条转发文本上限（shell 行输出远小于此） */

/* 用户态控制台输出（sys_debug_write -> 此处）。
 * 策略（窗口化纯合成器架构）：
 *   - 显示服务已接管帧缓冲（g_display_active）：display-server 是【纯合成器】，
 *     不渲染任何字符；各窗口应用自己把文本/图形绘制进离屏缓冲再经 OOL 提交。
 *     因此用户态诊断输出【只走串口】（headless 可观测），绝不写帧缓冲，避免
 *     覆盖合成桌面或越权绘制像素。
 *   - 显示未激活且帧缓冲可用（启动早期/纯文本回退）：直接 fbcon 写屏 + 串口。
 *   - 完全无图形（g_use_fb=false）：回退 VGA 文本 + 串口，保证文本不丢失。 */
void user_puts(const char *s)
{
    if (g_use_fb && !g_display_active) {
        fbcon_write(s);
    } else if (!g_use_fb) {
        vga_write(s);
    }
    serial_writestr(s);
}

/* 用户态 TTY 输出（fd 0/1/2 写 TTY 后端经 tty_write 调用）。
 * 策略（窗口化纯合成器架构，与 user_puts 互补）：
 *   - 显示服务已接管帧缓冲（g_display_active）：不写帧缓冲（避免覆盖合成桌面），
 *     而是捕获进「用户 TTY 环形管道」g_user_tty_pipe，由 shell 经 SYS_TTY_READ
 *     读回并渲染进自己的终端窗口（shell 作为终端）。
 *   - 显示未激活且帧缓冲可用（启动早期/纯文本回退）：直接 fbcon 写屏 + 串口。
 *   - 完全无图形（g_use_fb=false）：回退 VGA 文本 + 串口。
 *   - 任何情况串口恒定输出（headless 可观测、与历史行为一致）。 */
void user_tty_out(const char *s)
{
    if (g_display_active) {
        user_tty_pipe_write(s);          /* 交给 shell 终端渲染 */
    } else if (g_use_fb) {
        fbcon_write(s);                  /* 启动期/纯文本回退：直接写屏 */
    } else {
        vga_write(s);
    }
    serial_writestr(s);                  /* 串口恒定 */
}

/* 进入用户态服务前由 kmain 调用：关闭后内核运行期诊断只走串口，
 * 帧缓冲留给 shell。 */
void console_set_fb_diag(bool on)
{
    g_kernel_fb_diag = on;
}

void kprintf(const char *fmt, ...)
{
    uint64_t irqf = spin_lock_irqsave(&g_kp_lock);  /* 整条消息跨 CPU 原子输出 */
    if (g_kp_depth >= 4) {            /* M18：重入过深，放弃输出防递归死循环 */
        spin_unlock_irqrestore(&g_kp_lock, irqf);
        return;
    }
    g_kp_depth++;
    va_list ap;
    va_start(ap, fmt);
    /* 单条消息输出字符硬上限：若某条 kprintf 异常地想输出海量字符
     * （fmt 无终止符 / %s 指向损坏内存 / print_* 宽度溢出），超过此上限
     * 即强制截断并告警。避免持有 g_kp_lock 期间无限循环冻结整个系统。 */
    g_kprintf_nout = 0;
    const size_t KPRINTF_MAX = 1024;

    for (const char *p = fmt; *p; p++) {
        if (g_kprintf_nout >= KPRINTF_MAX) {
            /* 超限：打印诊断（含调用者返回地址，定位是哪条 kprintf），截断返回 */
            serial_writestr("\n[console] kprintf TRUNCATED caller=");
            serial_write_hex((uint64_t)(uintptr_t)__builtin_return_address(0));
            serial_writestr(" fmt=");
            for (int i = 0; i < 24 && fmt[i]; i++) {
                char c = fmt[i];
                serial_write(c >= 0x20 && c < 0x7f ? c : '.');
            }
            serial_writestr("\n");
            va_end(ap);
            g_kp_depth--;
            spin_unlock_irqrestore(&g_kp_lock, irqf);
            return;
        }
        if (*p != '%') {
            kputc(*p);
            continue;
        }
        p++;
        /* 简单支持 0 填充与宽度：如 %08x */
        char pad = ' ';
        int width = 0;
        if (*p == '0') {
            pad = '0';
            p++;
        }
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }
        /* 长度修饰符 l/ll 忽略处理（统一按 64 位取） */
        bool is_long = false;
        while (*p == 'l') {
            is_long = true;
            p++;
        }

        switch (*p) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            kputs(s ? s : "(null)");
            break;
        }
        case 'c':
            kputc((char)va_arg(ap, int));
            break;
        case 'd':
        case 'i':
            if (is_long) {
                print_int(va_arg(ap, int64_t));
            } else {
                print_int(va_arg(ap, int));
            }
            break;
        case 'u':
            if (is_long) {
                print_uint(va_arg(ap, uint64_t), 10, false, width, pad);
            } else {
                print_uint(va_arg(ap, unsigned int), 10, false, width, pad);
            }
            break;
        case 'x':
            if (is_long) {
                print_uint(va_arg(ap, uint64_t), 16, false, width, pad);
            } else {
                print_uint(va_arg(ap, unsigned int), 16, false, width, pad);
            }
            break;
        case 'X':
            if (is_long) {
                print_uint(va_arg(ap, uint64_t), 16, true, width, pad);
            } else {
                print_uint(va_arg(ap, unsigned int), 16, true, width, pad);
            }
            break;
        case 'p':
            kputs("0x");
            print_uint((uint64_t)va_arg(ap, void *), 16, false, 16, '0');
            break;
        case '%':
            kputc('%');
            break;
        case '\0':
            va_end(ap);
            g_kp_depth--;
            spin_unlock_irqrestore(&g_kp_lock, irqf);
            return;
        default:
            kputc('%');
            kputc(*p);
            break;
        }
    }
    va_end(ap);
    g_kp_depth--;
    spin_unlock_irqrestore(&g_kp_lock, irqf);
}

__attribute__((noreturn)) void panic(const char *fmt, ...)
{
    /* P0-3：先命令其它 CPU 停机（防半死状态跑坏数据），再强制重置输出锁——
     * 若 panic 恰发生在本 CPU 持有 g_kp_lock 的 kprintf 途中（如输出时 #PF），
     * ticket 锁不可重入会导致永久自旋。panic 不归路 + 他核已停，重置是安全的。 */
    smp_halt_others();
    __asm__ volatile("cli" ::: "memory");
    spinlock_init(&g_kp_lock, "kprintf");
    g_kp_depth = 0;

    /* panic 归路：强制在帧缓冲也输出诊断（即便此前已进入用户态关闭了
     * 内核 fb 镜像），确保致命错误在图形终端可见。 */
    g_kernel_fb_diag = true;

    va_list ap;
    kprintf("\n[PANIC] ");
    va_start(ap, fmt);
    /* 复用 kprintf 的格式能力：这里简单转发（不支持嵌套 va_list 的完整实现，
     * 直接以字符串方式输出常见用法即可）。 */
    for (const char *p = fmt; *p; p++) {
        if (*p == '%' && *(p + 1)) {
            p++;
            switch (*p) {
            case 's': kputs(va_arg(ap, const char *)); break;
            case 'd': print_int(va_arg(ap, int)); break;
            case 'x': print_uint(va_arg(ap, unsigned int), 16, false, 0, ' '); break;
            case 'p': kputs("0x"); print_uint((uint64_t)va_arg(ap, void *), 16, false, 16, '0'); break;
            case 'l': {
                /* %lx / %ld */
                p++;
                if (*p == 'x') print_uint(va_arg(ap, uint64_t), 16, false, 0, ' ');
                else print_int(va_arg(ap, int64_t));
                break;
            }
            default: kputc('%'); kputc(*p); break;
            }
        } else {
            kputc(*p);
        }
    }
    va_end(ap);
    kprintf("\n[PANIC] system halted.\n");
    diag_dump_self();            /* P0-9：打印当前执行流的栈回溯，便于事后定位 */

    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
