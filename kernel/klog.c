/*
 * kernel/klog.c
 * -----------------------------------------------------------------------------
 * P0-R3a：早期启动日志环缓冲实现（设计见 include/kernel/klog.h）。
 *
 * 并发模型：g_head 用 __atomic_fetch_add 取号——每个字符获得全局唯一序号，
 * 写入槽位 head & (SIZE-1)。多核同时 kprintf 时字符粒度交错与串口一致
 * （串口输出本身由 kprintf 锁串行化，klog 在锁内被调用，顺序完全一致）。
 *
 * 重放抑制：klog_dump 经 serial_write 输出，而 serial_write 又会调
 * klog_putc——用 per-dump 抑制标志切断递归，避免重放内容再次入环。
 */
#include <kernel/klog.h>
#include <kernel/serial.h>

/* 非 static：固定符号，便于 GDB / QEMU monitor 崩溃后直接读内存取证 */
char     g_klog_buf[KLOG_BUF_SIZE];
uint64_t g_klog_head;                  /* 累计写入总字节数（含被覆盖的） */

static volatile int g_klog_suspend;    /* klog_dump 重放期间抑制记录 */

void klog_putc(char c)
{
    if (g_klog_suspend) {
        return;
    }
    uint64_t seq = __atomic_fetch_add(&g_klog_head, 1, __ATOMIC_RELAXED);
    g_klog_buf[seq & (KLOG_BUF_SIZE - 1)] = c;
}

uint64_t klog_written(void)
{
    return __atomic_load_n(&g_klog_head, __ATOMIC_RELAXED);
}

uint32_t klog_avail(void)
{
    uint64_t head = klog_written();
    return (head >= KLOG_BUF_SIZE) ? KLOG_BUF_SIZE : (uint32_t)head;
}

char klog_at(uint32_t i)
{
    uint64_t head = klog_written();
    uint64_t avail = (head >= KLOG_BUF_SIZE) ? KLOG_BUF_SIZE : head;
    if (i >= avail) {
        return '\0';
    }
    /* 时间序最旧字符位于 head-avail 序号处 */
    return g_klog_buf[(head - avail + i) & (KLOG_BUF_SIZE - 1)];
}

void klog_dump(void)
{
    g_klog_suspend = 1;
    serial_writestr("\n===== klog ring buffer dump (");
    serial_write_dec(klog_written());
    serial_writestr(" bytes total) =====\n");
    uint32_t n = klog_avail();
    for (uint32_t i = 0; i < n; i++) {
        serial_write(klog_at(i));
    }
    serial_writestr("\n===== end of klog dump =====\n");
    g_klog_suspend = 0;
}
