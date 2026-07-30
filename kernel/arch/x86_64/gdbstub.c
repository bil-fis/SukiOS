/*
 * kernel/arch/x86_64/gdbstub.c
 * -----------------------------------------------------------------------------
 * P0-R3c：串口 GDB stub —— GDB Remote Serial Protocol (RSP) 实现。
 * 参考：osdev wiki "GDB"/"Kernel Debugging" 条目 + GDB 手册附录 E（协议）。
 *
 * 传输层：COM2 (0x2F8) 轮询式 16550，与 COM1（内核日志）完全隔离。
 * 包格式：$<data>#<2位十六进制校验和>，校验和 = data 字节和 mod 256；
 *         收到合法包回 '+'，损坏回 '-'（GDB 会重传）。
 *
 * 陷入路径：
 *   - GDB 侧断点：GDB 经 M 包往目标地址写 0xCC，命中 -> #BP(vec3) -> 本 stub
 *     （GDB 自行识别「PC-1 是自己种的断点」并回退 PC，stub 不调 RIP）；
 *   - 单步：s 包置 RFLAGS.TF，下一条指令后 #DB(vec1) -> 本 stub（入口清 TF）；
 *   - 远程主动附着：sched_tick 轮询 COM2 RX（gdbstub_poll），有数据即执行
 *     int3 自陷，GDB 的首包由会话循环应答。
 *
 * 寄存器视图（GDB amd64 缺省描述，g 包定长 536 字节）：
 *   rax,rbx,rcx,rdx,rsi,rdi,rbp,rsp,r8..r15,rip（各 8B，小端）
 *   eflags,cs,ss,ds,es,fs,gs（各 4B）
 *   st0-7/fctrl../xmm0-15/mxcsr —— 本内核不在中断帧保存 FPU，以 'x' 填充
 *   （RSP 允许用 'x' 表示寄存器值不可用，GDB 正常接受）。
 *
 * 内存访问安全：m/M 包逐页经 vmm_translate(当前 CR3 根) 校验映射后，
 * 通过物理直映窗口 PHYS_TO_VIRT 读写——绕开 SMAP（用户页）与 W^X（内核
 * .text 种断点需要写），未映射页返回 E14，stub 自身绝不触发缺页。
 *
 * SMP 一致性：会话期间置 g_gdb_freeze 并广播 IPI_GDB_FREEZE，其它 CPU 在
 * 中断上下文自旋（IF=0），寄存器/内存快照不漂移；continue/step/detach 释放。
 */
#include <kernel/gdbstub.h>
#include <kernel/serial.h>
#include <kernel/io.h>
#include <kernel/console.h>
#include <kernel/interrupts.h>
#include <kernel/smp.h>
#include <kernel/apic.h>
#include <kernel/percpu.h>
#include <kernel/klog.h>
#include <mm/vmm.h>

/* ---------------- COM2 轮询式 16550 ---------------- */
#define COM2_BASE   0x2F8
#define UART_DATA   0
#define UART_IER    1
#define UART_FCR    2
#define UART_LCR    3
#define UART_MCR    4
#define UART_LSR    5
#define LSR_RX_READY 0x01
#define LSR_THR_EMPTY 0x20

#define RFLAGS_TF   0x100UL

static bool g_gdb_inited;
static volatile int g_gdb_active;    /* 会话进行中（防重入/轮询自陷） */
static volatile int g_gdb_freeze;    /* 其它 CPU 冻结标志 */
static bool g_gdb_attached;          /* 已完成一次会话握手 */

/* RSP 包缓冲：PacketSize 上报 0x1000（4096） */
#define GDB_BUF_SIZE 4096
static char g_inbuf[GDB_BUF_SIZE + 1];
static char g_outbuf[GDB_BUF_SIZE + 16];

static void com2_init(void)
{
    outb(COM2_BASE + UART_IER, 0x00);
    outb(COM2_BASE + UART_LCR, 0x80);
    outb(COM2_BASE + UART_DATA, 0x01);   /* 115200 */
    outb(COM2_BASE + UART_IER, 0x00);
    outb(COM2_BASE + UART_LCR, 0x03);
    outb(COM2_BASE + UART_FCR, 0xC7);
    outb(COM2_BASE + UART_MCR, 0x0B);
}

static bool com2_rx_ready(void)
{
    return (inb(COM2_BASE + UART_LSR) & LSR_RX_READY) != 0;
}

static uint8_t com2_getc(void)
{
    while (!com2_rx_ready()) {
        __asm__ volatile("pause");
    }
    return inb(COM2_BASE + UART_DATA);
}

static void com2_putc(uint8_t c)
{
    while (!(inb(COM2_BASE + UART_LSR) & LSR_THR_EMPTY)) {
        __asm__ volatile("pause");
    }
    outb(COM2_BASE + UART_DATA, c);
}

/* ---------------- hex 工具 ---------------- */
static const char g_hexchars[] = "0123456789abcdef";

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 解析十六进制数，*p 前进到首个非 hex 字符；返回是否至少 1 位 */
static bool parse_hex_u64(const char **p, uint64_t *out)
{
    uint64_t v = 0;
    int n = 0;
    while (hex_val(**p) >= 0) {
        v = (v << 4) | (uint64_t)hex_val(**p);
        (*p)++;
        n++;
    }
    *out = v;
    return n > 0;
}

/* ---------------- RSP 收发 ---------------- */

/* 接收一个合法包到 g_inbuf（去掉 $...#cs 封装），返回长度；
 * 自动应答 +/-。0x03（GDB Ctrl-C）在会话中到达时忽略（目标已停）。 */
static size_t gdb_recv_packet(void)
{
    for (;;) {
        uint8_t c = com2_getc();
        if (c != '$') {
            continue;                    /* 忽略 +/-/0x03/噪声 */
        }
        size_t len = 0;
        uint8_t sum = 0;
        bool overflow = false;
        for (;;) {
            c = com2_getc();
            if (c == '$') {              /* 意外重新开包 */
                len = 0; sum = 0; overflow = false;
                continue;
            }
            if (c == '#') {
                break;
            }
            sum = (uint8_t)(sum + c);
            if (len < GDB_BUF_SIZE) {
                g_inbuf[len++] = (char)c;
            } else {
                overflow = true;
            }
        }
        uint8_t rc = 0;
        int h = hex_val((char)com2_getc());
        int l = hex_val((char)com2_getc());
        if (h >= 0 && l >= 0) {
            rc = (uint8_t)((h << 4) | l);
        }
        if (!overflow && rc == sum) {
            com2_putc('+');
            g_inbuf[len] = '\0';
            return len;
        }
        com2_putc('-');                  /* 校验失败：请求重传 */
    }
}

/* 发送包并等待 '+' 确认（'-' 则重发；读到 '$' 说明 GDB 已放弃确认直接发
 * 新包——按新式无 ack 兼容处理，把 '$' 留给上层不可行，这里简单重收） */
static void gdb_send_packet(const char *data, size_t len)
{
    for (int attempt = 0; attempt < 8; attempt++) {
        uint8_t sum = 0;
        com2_putc('$');
        for (size_t i = 0; i < len; i++) {
            sum = (uint8_t)(sum + (uint8_t)data[i]);
            com2_putc((uint8_t)data[i]);
        }
        com2_putc('#');
        com2_putc((uint8_t)g_hexchars[(sum >> 4) & 0xF]);
        com2_putc((uint8_t)g_hexchars[sum & 0xF]);

        uint8_t c = com2_getc();
        if (c == '+') {
            return;
        }
        if (c != '-') {
            return;                      /* 非重传请求（如 0x03）：不纠缠 */
        }
    }
}

static void gdb_send_str(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    gdb_send_packet(s, n);
}

/* ---------------- 安全内存访问（经物理直映） ---------------- */

static uint64_t current_pml4_root(void)
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    /* 中断入口已切内核视图（KPTI 时 bit12=0）；掩出物理基址 */
    return cr3 & PTE_ADDR_MASK;
}

/* 读 n 字节到 dst；任一页未映射返回 false（不产生缺页） */
static bool safe_mem_read(uint64_t va, uint8_t *dst, size_t n)
{
    uint64_t root = current_pml4_root();
    while (n > 0) {
        uint64_t pa = vmm_translate(root, va & ~(PAGE_SIZE - 1));
        if (!pa) {
            return false;
        }
        size_t off = (size_t)(va & (PAGE_SIZE - 1));
        size_t chunk = PAGE_SIZE - off;
        if (chunk > n) chunk = n;
        const uint8_t *src = (const uint8_t *)PHYS_TO_VIRT(pa + off);
        for (size_t i = 0; i < chunk; i++) {
            dst[i] = src[i];
        }
        dst += chunk;
        va  += chunk;
        n   -= chunk;
    }
    return true;
}

/* 写 n 字节（直映别名写，绕开 .text 只读映射以支持种 0xCC 断点） */
static bool safe_mem_write(uint64_t va, const uint8_t *src, size_t n)
{
    uint64_t root = current_pml4_root();
    while (n > 0) {
        uint64_t pa = vmm_translate(root, va & ~(PAGE_SIZE - 1));
        if (!pa) {
            return false;
        }
        size_t off = (size_t)(va & (PAGE_SIZE - 1));
        size_t chunk = PAGE_SIZE - off;
        if (chunk > n) chunk = n;
        uint8_t *dst = (uint8_t *)PHYS_TO_VIRT(pa + off);
        for (size_t i = 0; i < chunk; i++) {
            dst[i] = src[i];
        }
        src += chunk;
        va  += chunk;
        n   -= chunk;
    }
    /* 写的可能是指令字节：无自修改代码序列化需求之外，QEMU/TCG 与真机
     * 对断点字节的可见性由 iretq（串行化）保证。 */
    return true;
}

/* ---------------- 寄存器打包（GDB amd64 缺省布局，536 字节） ---------------- */
#define GDB_NUM_REGS64 17                /* rax..r15 + rip */
#define GDB_G_BYTES    536               /* 上述布局总字节数 */

/* 把 8 字节值按小端追加为 16 个 hex 字符 */
static char *put_hex_le(char *p, uint64_t v, int bytes)
{
    for (int i = 0; i < bytes; i++) {
        uint8_t b = (uint8_t)(v >> (i * 8));
        *p++ = g_hexchars[(b >> 4) & 0xF];
        *p++ = g_hexchars[b & 0xF];
    }
    return p;
}

/* 按索引取寄存器指针（0..16 = 8 字节 GPR/rip；17..23 = 4 字节段/标志） */
static uint64_t *reg_slot64(registers_t *r, int idx)
{
    switch (idx) {
    case 0:  return &r->rax;
    case 1:  return &r->rbx;
    case 2:  return &r->rcx;
    case 3:  return &r->rdx;
    case 4:  return &r->rsi;
    case 5:  return &r->rdi;
    case 6:  return &r->rbp;
    case 7:  return &r->rsp;
    case 8:  return &r->r8;
    case 9:  return &r->r9;
    case 10: return &r->r10;
    case 11: return &r->r11;
    case 12: return &r->r12;
    case 13: return &r->r13;
    case 14: return &r->r14;
    case 15: return &r->r15;
    case 16: return &r->rip;
    default: return NULL;
    }
}

/* 17..23：eflags,cs,ss,ds,es,fs,gs（内核帧无 ds/es/fs/gs，用 ss 值近似
 * ——长模式下数据段选择子不影响寻址，仅供 GDB 显示） */
static uint64_t reg_read32(registers_t *r, int idx)
{
    switch (idx) {
    case 17: return r->rflags;
    case 18: return r->cs;
    case 19: return r->ss;
    case 20: case 21: case 22: case 23: return r->ss;
    default: return 0;
    }
}

static void cmd_read_regs(registers_t *r)
{
    char *p = g_outbuf;
    for (int i = 0; i < GDB_NUM_REGS64; i++) {
        p = put_hex_le(p, *reg_slot64(r, i), 8);
    }
    for (int i = 17; i <= 23; i++) {
        p = put_hex_le(p, reg_read32(r, i), 4);
    }
    /* FPU/SSE 区（st0-7 + 控制字 + xmm + mxcsr）：'x' = 不可用 */
    for (int i = 0; i < (GDB_G_BYTES - 164) * 2; i++) {
        *p++ = 'x';
    }
    gdb_send_packet(g_outbuf, (size_t)(p - g_outbuf));
}

static void cmd_write_regs(registers_t *r, const char *data, size_t len)
{
    /* 只接受完整 GPR 区（17*16=272 hex 字符）+ 可选后续；'x' 表示不改 */
    size_t need = GDB_NUM_REGS64 * 16;
    if (len < need) {
        gdb_send_str("E01");
        return;
    }
    const char *p = data;
    for (int i = 0; i < GDB_NUM_REGS64; i++) {
        uint64_t v = 0;
        bool skip = false;
        for (int b = 0; b < 8; b++) {
            int h = hex_val(p[b * 2]);
            int l = hex_val(p[b * 2 + 1]);
            if (h < 0 || l < 0) {        /* 'x'：该寄存器不修改 */
                skip = true;
                break;
            }
            v |= ((uint64_t)((h << 4) | l)) << (b * 8);
        }
        if (!skip) {
            *reg_slot64(r, i) = v;
        }
        p += 16;
    }
    /* eflags（若提供）：只允许改 TF/IF 之外的算术位？——调试器语义上应
     * 全量可改，但强制保留 IF 状态防止把中断状态调坏。 */
    if (len >= need + 8) {
        uint64_t v = 0;
        bool ok = true;
        for (int b = 0; b < 4; b++) {
            int h = hex_val(p[b * 2]);
            int l = hex_val(p[b * 2 + 1]);
            if (h < 0 || l < 0) { ok = false; break; }
            v |= ((uint64_t)((h << 4) | l)) << (b * 8);
        }
        if (ok) {
            uint64_t keep_if = r->rflags & 0x200UL;
            r->rflags = (v & ~0x200UL) | keep_if;
        }
    }
    gdb_send_str("OK");
}

static void cmd_read_mem(const char *args)
{
    const char *p = args;
    uint64_t addr, len;
    if (!parse_hex_u64(&p, &addr) || *p != ',' ||
        (p++, !parse_hex_u64(&p, &len))) {
        gdb_send_str("E00");
        return;
    }
    if (len > (GDB_BUF_SIZE - 16) / 2) {
        len = (GDB_BUF_SIZE - 16) / 2;
    }
    static uint8_t tmp[GDB_BUF_SIZE / 2];
    if (!safe_mem_read(addr, tmp, (size_t)len)) {
        gdb_send_str("E14");             /* EFAULT：页未映射 */
        return;
    }
    char *o = g_outbuf;
    for (uint64_t i = 0; i < len; i++) {
        *o++ = g_hexchars[(tmp[i] >> 4) & 0xF];
        *o++ = g_hexchars[tmp[i] & 0xF];
    }
    gdb_send_packet(g_outbuf, (size_t)(o - g_outbuf));
}

static void cmd_write_mem(const char *args)
{
    const char *p = args;
    uint64_t addr, len;
    if (!parse_hex_u64(&p, &addr) || *p != ',' ||
        (p++, !parse_hex_u64(&p, &len)) || *p != ':') {
        gdb_send_str("E00");
        return;
    }
    p++;                                 /* 跳过 ':' */
    static uint8_t tmp[GDB_BUF_SIZE / 2];
    if (len > sizeof(tmp)) {
        gdb_send_str("E01");
        return;
    }
    for (uint64_t i = 0; i < len; i++) {
        int h = hex_val(p[i * 2]);
        int l = hex_val(p[i * 2 + 1]);
        if (h < 0 || l < 0) {
            gdb_send_str("E00");
            return;
        }
        tmp[i] = (uint8_t)((h << 4) | l);
    }
    if (!safe_mem_write(addr, tmp, (size_t)len)) {
        gdb_send_str("E14");
        return;
    }
    gdb_send_str("OK");
}

/* qRcmd 命令解码缓冲 */
static char g_rcmd[64];

/* monitor 命令输出：hex 编码为 O 包分块发送 */
static void rcmd_emit_text(const char *s, size_t n)
{
    while (n > 0) {
        size_t chunk = n > 120 ? 120 : n;
        char *o = g_outbuf;
        *o++ = 'O';
        for (size_t i = 0; i < chunk; i++) {
            uint8_t b = (uint8_t)s[i];
            *o++ = g_hexchars[(b >> 4) & 0xF];
            *o++ = g_hexchars[b & 0xF];
        }
        gdb_send_packet(g_outbuf, (size_t)(o - g_outbuf));
        s += chunk;
        n -= chunk;
    }
}

static void cmd_qrcmd(const char *hexcmd)
{
    size_t n = 0;
    while (hexcmd[0] && hexcmd[1] && n < sizeof(g_rcmd) - 1) {
        int h = hex_val(hexcmd[0]);
        int l = hex_val(hexcmd[1]);
        if (h < 0 || l < 0) break;
        g_rcmd[n++] = (char)((h << 4) | l);
        hexcmd += 2;
    }
    g_rcmd[n] = '\0';

    if (n == 4 && g_rcmd[0]=='k' && g_rcmd[1]=='l' && g_rcmd[2]=='o' &&
        g_rcmd[3]=='g') {
        /* monitor klog：把启动日志环缓冲全部内容传回 GDB 控制台 */
        uint32_t avail = klog_avail();
        static char line[121];
        size_t li = 0;
        for (uint32_t i = 0; i < avail; i++) {
            line[li++] = klog_at(i);
            if (li == 120) {
                rcmd_emit_text(line, li);
                li = 0;
            }
        }
        if (li > 0) {
            rcmd_emit_text(line, li);
        }
        gdb_send_str("OK");
        return;
    }
    gdb_send_str("");                    /* 未知 monitor 命令 */
}

/* ---------------- SMP 冻结 ---------------- */

static void gdb_freeze_handler(registers_t *r)
{
    (void)r;
    /* 中断上下文（IF=0）自旋直到 stub 释放；pause 降功耗/让出流水线 */
    while (__atomic_load_n(&g_gdb_freeze, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }
}

static void freeze_others(void)
{
    if (smp_online_count() > 1) {
        __atomic_store_n(&g_gdb_freeze, 1, __ATOMIC_RELEASE);
        lapic_broadcast_ipi(IPI_GDB_FREEZE);
    }
}

static void unfreeze_others(void)
{
    __atomic_store_n(&g_gdb_freeze, 0, __ATOMIC_RELEASE);
}

/* ---------------- 会话主循环 ---------------- */

/* 返回值：true=恢复执行（c/s/D/k），false 不出现（循环内处理一切） */
static void gdb_session(registers_t *r)
{
    /* 停机原因：统一 SIGTRAP(5)。 */
    gdb_send_str("S05");

    for (;;) {
        size_t len = gdb_recv_packet();
        if (len == 0) {
            gdb_send_str("");
            continue;
        }
        char c = g_inbuf[0];
        const char *args = &g_inbuf[1];

        switch (c) {
        case '?':
            gdb_send_str("S05");
            break;
        case 'g':
            cmd_read_regs(r);
            break;
        case 'G':
            cmd_write_regs(r, args, len - 1);
            break;
        case 'm':
            cmd_read_mem(args);
            break;
        case 'M':
            cmd_write_mem(args);
            break;
        case 'p': {                      /* 读单个寄存器 */
            const char *p = args;
            uint64_t idx;
            if (!parse_hex_u64(&p, &idx)) {
                gdb_send_str("E00");
                break;
            }
            char *o = g_outbuf;
            if (idx <= 16) {
                o = put_hex_le(o, *reg_slot64(r, (int)idx), 8);
            } else if (idx <= 23) {
                o = put_hex_le(o, reg_read32(r, (int)idx), 4);
            } else {
                gdb_send_str("E01");     /* FPU/SSE：不可用 */
                break;
            }
            gdb_send_packet(g_outbuf, (size_t)(o - g_outbuf));
            break;
        }
        case 'P': {                      /* 写单个寄存器：Pn=v */
            const char *p = args;
            uint64_t idx;
            if (!parse_hex_u64(&p, &idx) || *p != '=') {
                gdb_send_str("E00");
                break;
            }
            p++;
            uint64_t v = 0;
            for (int b = 0; b < 8 && hex_val(p[b*2]) >= 0 &&
                            hex_val(p[b*2+1]) >= 0; b++) {
                v |= ((uint64_t)((hex_val(p[b*2]) << 4) |
                                 hex_val(p[b*2+1]))) << (b * 8);
            }
            if (idx <= 16) {
                *reg_slot64(r, (int)idx) = v;
                gdb_send_str("OK");
            } else if (idx == 17) {
                uint64_t keep_if = r->rflags & 0x200UL;
                r->rflags = (v & ~0x200UL) | keep_if;
                gdb_send_str("OK");
            } else {
                gdb_send_str("E01");
            }
            break;
        }
        case 'c':                        /* 继续 */
            r->rflags &= ~RFLAGS_TF;
            return;
        case 's':                        /* 单步：置 TF，下一条后 #DB 再入 */
            r->rflags |= RFLAGS_TF;
            return;
        case 'H':                        /* 线程选择：单线程视图，恒 OK */
            gdb_send_str("OK");
            break;
        case 'T':                        /* 线程存活查询 */
            gdb_send_str("OK");
            break;
        case 'q':
            if (len >= 10 && __builtin_memcmp(args, "Supported", 9) == 0) {
                gdb_send_str("PacketSize=1000");
            } else if (len >= 9 && __builtin_memcmp(args, "Attached", 8) == 0) {
                gdb_send_str("1");       /* 附着到既存系统（detach 不杀） */
            } else if (len >= 5 && __builtin_memcmp(args, "Rcmd,", 5) == 0) {
                cmd_qrcmd(args + 5);
            } else if (len >= 2 && args[0] == 'C') {
                gdb_send_str("QC0");     /* 当前线程 */
            } else if (len >= 12 &&
                       __builtin_memcmp(args, "fThreadInfo", 11) == 0) {
                gdb_send_str("m0");
            } else if (len >= 12 &&
                       __builtin_memcmp(args, "sThreadInfo", 11) == 0) {
                gdb_send_str("l");
            } else {
                gdb_send_str("");        /* 未支持的查询：空回复 */
            }
            break;
        case 'D':                        /* detach：恢复运行 */
            gdb_send_str("OK");
            r->rflags &= ~RFLAGS_TF;
            g_gdb_attached = false;
            return;
        case 'k':                        /* kill：对内核即「继续运行」 */
            r->rflags &= ~RFLAGS_TF;
            g_gdb_attached = false;
            return;
        case 'z':                        /* 断点管理：回空 -> GDB 用软断点(M) */
        case 'Z':
            gdb_send_str("");
            break;
        default:
            gdb_send_str("");            /* 未知命令：RSP 规定空回复 */
            break;
        }
    }
}

/* #DB(vec1)/#BP(vec3) 共用陷入入口 */
static void gdb_trap(registers_t *r)
{
    if (r->int_no == 1) {
        r->rflags &= ~RFLAGS_TF;         /* 单步落地：清 TF 防止连环 #DB */
    }
    if (g_gdb_active) {
        return;                          /* 会话重入（理论不可达）：忽略 */
    }
    g_gdb_active = 1;

    if (!g_gdb_attached) {
        /* 首次陷入：向 COM1 日志留痕，便于操作者知道系统停在调试器里 */
        kprintf("[gdb] trap vec=%lu rip=%p — GDB session on COM2 "
                "(target remote; symbol-file with KASLR slide)\n",
                (unsigned long)r->int_no, (void *)r->rip);
        g_gdb_attached = true;
    }

    freeze_others();
    gdb_session(r);
    unfreeze_others();

    g_gdb_active = 0;
}

void gdbstub_init(void)
{
    com2_init();
    register_interrupt_handler(1, gdb_trap);    /* #DB：单步/硬件断点 */
    register_interrupt_handler(3, gdb_trap);    /* #BP：int3 软断点 */
    register_interrupt_handler(IPI_GDB_FREEZE, gdb_freeze_handler);
    g_gdb_inited = true;
    kprintf("[gdb] stub ready on COM2 (0x2F8): vec1/vec3 hooked, "
            "freeze IPI=0x%x, attach anytime via COM2\n", IPI_GDB_FREEZE);
}

void gdbstub_poll(void)
{
    if (!g_gdb_inited || g_gdb_active || cpu_index() != 0) {
        return;
    }
    /* 只认 GDB 特征字节才自陷：'$'（RSP 包首）、0x03（GDB Ctrl-C 中断）、
     * '+'（ack，GDB 附着时常先发一个）。其余（TCP 后端断连噪声/0x00/0xFF）
     * 直接丢弃——否则杂散字节会把系统误挂进调试会话。丢掉的 '$' 不丢包：
     * 会话循环对损坏包回 '-'，GDB 超时自动重传。 */
    for (int drain = 0; drain < 16 && com2_rx_ready(); drain++) {
        uint8_t b = inb(COM2_BASE + UART_DATA);
        if (b == '$' || b == 0x03 || b == '+') {
            /* int3 自陷进入会话；GDB 的完整首包（含刚被消费的字节）由
             * 重传机制补齐，握手自然完成。 */
            __asm__ volatile("int3");
            return;
        }
    }
}
