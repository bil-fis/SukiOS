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
/* M18 修复：kprintf 重入深度计数。单核下 irq_save 已保证一条消息原子输出，
 * 但若在输出途中触发 #PF 等异常二次进入 kprintf（如 panic 路径），会递归
 * 打印导致栈耗尽。超过阈值即放弃本次输出，既保证普通场景正常又防致命递归。
 * SMP 后的跨 CPU 串行化自旋锁随 P0-3 引入。 */
static volatile int g_kp_depth = 0;

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
    serial_write(c);
    /* 内核诊断是否镜像到帧缓冲：进入用户态后由 console_set_fb_diag(false)
     * 关闭，避免 [ipc]/[sched] 等运行期日志刷屏图形终端（shell 专用）。 */
    if (g_use_fb && g_kernel_fb_diag) {
        fbcon_putc(c);
    } else {
        vga_putc(c);
    }
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
 * 策略：
 *   - 显示服务已激活（g_display_active）且帧缓冲可用：把文本经 IPC 转交显示服务，
 *     由它在桌面内合成「终端窗口」渲染，内核【不再直接写帧缓冲】——否则会覆盖
 *     显示服务已经画好的合成桌面（这正是「画面还是控制台 shell」的根因）。
 *   - 未激活或转发失败（端口队列满/内存紧张）：降级为直接 fbcon/vga + 串口，
 *     保证文本在任何情况下都不丢失。
 * 串口日志恒定输出，便于无图形场景也能看到 Ring3 输出。 */
void user_puts(const char *s)
{
    if (g_display_active && g_use_fb) {
        char buf[USER_PUTS_MAX];
        uint32_t n = 0;
        for (const char *p = s; *p && n < USER_PUTS_MAX - 1; p++) {
            buf[n++] = *p;
        }
        buf[n] = 0;

        uint8_t msg[sizeof(mach_msg_header_t) + USER_PUTS_MAX];
        memset(msg, 0, sizeof(msg));
        mach_msg_header_t *h = (mach_msg_header_t *)msg;
        h->msgh_bits        = 0;
        h->msgh_size        = sizeof(*h) + (uint32_t)strlen(buf) + 1;
        h->msgh_remote_port = DISPLAY_PORT;
        h->msgh_local_port  = PORT_NULL;
        h->msgh_id          = DISP_MSG_TEXT;
        h->msgh_reserved    = 0;
        memcpy(msg + sizeof(*h), buf, n + 1);

        if (ipc_send_kernel(DISPLAY_PORT, msg, h->msgh_size) == MACH_MSG_SUCCESS) {
            serial_writestr(s);   /* 转发成功：仅串口留底，避免再写屏覆盖桌面 */
            return;
        }
        /* 转发失败（队列满/未就绪）：降级直接写屏 + 串口 */
    }

    if (g_use_fb) {
        fbcon_write(s);
    } else {
        vga_write(s);
    }
    serial_writestr(s);
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
