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
 */
#include <kernel/console.h>
#include <kernel/serial.h>
#include <kernel/framebuffer.h>
#include <kernel/vga_text.h>
#include <kernel/diagnostics.h>
#include <kernel/spinlock.h>
#include <kernel/smp.h>
#include <stdarg.h>

/* P0-3：kprintf 跨 CPU 串行化自旋锁。spin_lock_irqsave = 关本地中断 +
 * ticket 锁，单核下锁必然立即成功（退化为原 cli/sti 行为，零额外开销），
 * SMP 下保证一条消息跨 CPU 原子输出（BSP 日志 vs AP 启动日志不撕裂）。 */
static spinlock_t g_kp_lock = SPINLOCK_INIT("kprintf");

static bool g_use_fb = false;
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

void kputc(char c)
{
    serial_write(c);
    if (g_use_fb) {
        fbcon_putc(c);
    } else {
        vga_putc(c);
    }
}

void kputs(const char *s)
{
    while (*s) {
        kputc(*s++);
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

    for (const char *p = fmt; *p; p++) {
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
