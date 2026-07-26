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
#include <stdarg.h>

/* 保存并关闭中断 / 恢复：保证一条 kprintf 输出的原子性（单核足够） */
static inline uint64_t irq_save(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void irq_restore(uint64_t flags)
{
    if (flags & (1UL << 9)) {          /* 原 IF=1 才恢复开中断 */
        __asm__ volatile("sti" ::: "memory");
    }
}

static bool g_use_fb = false;

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
    uint64_t irqf = irq_save();       /* 整条消息原子输出，防多任务撕裂 */
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
            irq_restore(irqf);
            return;
        default:
            kputc('%');
            kputc(*p);
            break;
        }
    }
    va_end(ap);
    irq_restore(irqf);
}

__attribute__((noreturn)) void panic(const char *fmt, ...)
{
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

    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
