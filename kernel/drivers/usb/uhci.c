/*
 * kernel/drivers/usb/uhci.c
 * -----------------------------------------------------------------------------
 * UHCI (USB 1.1) 主机控制器驱动。
 *
 * 严格依据 OSDev Wiki「UHCI」条目与 Intel UHCI 规范实现：
 *   - PCI 探测（class 0x0C subclass 0x03 prog-if 0x00），禁用 BIOS legacy，
 *     使能 I/O + Bus Master，读 I/O BAR0。
 *   - 复位（HCRESET 自清 + GRESET），分配 4KB 对齐帧列表（1024 项），
 *     写 FRBASEADD，SOFMOD=0x40，USBCMD = RS|MAXP。
 *   - 帧列表 -> 控制 QH -> 中断 QH 链（每帧都执行）。
 *   - 控制传输：QH->SETUP TD -> DATA TD*n -> STATUS TD（深度优先链）。
 *   - 中断 IN：QH->单 TD，完成后轮询读取并按需翻转 data toggle 重新激活。
 *
 * 红线：DMA 一律使用 pmm_alloc_page()（物理页）+ PHYS_TO_VIRT；TD/QH 全部
 * 物理地址字段取自 VIRT_TO_PHYS，绝不把高半区虚拟地址写入硬件结构。
 */
#include <kernel/types.h>
#include <kernel/uhci.h>
#include <kernel/usb.h>
#include <kernel/io.h>
#include <kernel/pci.h>
#include <kernel/console.h>
#include <kernel/string.h>
#include <kernel/task.h>      /* msleep：长延时可让出 CPU（非忙等） */
#include <mm/pmm.h>

/* ---- QH / TD 硬件结构（16/32 字节，必须 16 字节对齐） ---- */
typedef struct __attribute__((packed, aligned(16))) {
    uint32_t horiz;      /* 水平指针（指向下一 QH/TD） */
    uint32_t vert;       /* 垂直指针（指向本 QH 的第一个 TD） */
    uint32_t reserved[2];
} uhci_qh_t;             /* 16 字节 */

typedef struct __attribute__((packed, aligned(16))) {
    uint32_t next;       /* 链接指针 + 标志（bit0 终止, bit2 深度优先） */
    uint32_t status;     /* CS：实际长度/错误/活动/IOC/低速/SPD */
    uint32_t token;      /* PID/dev/endpoint/toggle/maxlen */
    uint32_t buffer;     /* 数据缓冲物理地址 */
    uint32_t sw[4];      /* 系统保留 */
} uhci_td_t;             /* 32 字节 */

#define UHCI_MAX_CTRL_TD  80   /* 1 setup + 数据段(<=64) + 1 status 的余量 */

typedef struct {
    int       active;
    uint8_t   addr, ep, low_speed;
    uint16_t  mps, len;
    uint8_t  *buf_virt;     /* 调用方缓冲（内核虚拟） */
    uint64_t  dma_phys;     /* 本槽位结构页物理 */
    uint8_t  *dma_virt;
    uhci_qh_t *qh;
    uhci_td_t *td;
    uint8_t  *data;         /* 页内数据缓冲 */
    int       toggle;
} uhci_int_slot_t;

/* ---- 全局状态 ---- */
static bool     g_ready = false;
static uint16_t g_io = 0;
static int      g_ports = 0;
static uint64_t g_frame_phys = 0;
static uint32_t *g_frame = NULL;
static uint64_t g_ctrl_phys = 0;
static uint8_t *g_ctrl = NULL;      /* 结构页：TDs + setup */
static uhci_td_t *g_td = NULL;
static uint8_t  *g_setup = NULL;
static uint64_t g_data_phys = 0;
static uint8_t *g_data = NULL;      /* 控制传输数据缓冲页 */
static uint32_t g_int_chain = UHCI_PTR_TERM;
static uhci_int_slot_t g_int[UHCI_MAX_INT_SLOTS];
static uint8_t  g_port_ls[8];       /* 根端口低速标志缓存 */

/* ---- 端口 I/O 辅助（g_io 为已屏蔽低位的 I/O 基址） ---- */
static inline uint16_t ur16(uint16_t off) { return inw((uint16_t)(g_io + off)); }
static inline void     uw16(uint16_t off, uint16_t v) { outw((uint16_t)(g_io + off), v); }
static inline uint32_t ur32(uint16_t off) { return inl((uint16_t)(g_io + off)); }
static inline void     uw32(uint16_t off, uint32_t v) { outl((uint16_t)(g_io + off), v); }

/* ~1us 微延迟：读一次 0x80 端口（与 hda.c/smp.c 同法） */
static void usb_udelay(uint32_t us)
{
    for (uint32_t i = 0; i < us; i++) {
        (void)inb(0x80);
    }
}

/* 长延时可让出 CPU（用户要求：“USB 不要忙等，尽快让出 CPU”）。
 * 轮询任务上下文（USB 主机服务任务运行后）用内核 msleep 真睡眠；引导早期
 * （kmain 里 uhci_init 复位控制器时）尚无“可睡眠的任务上下文”，退化为微延迟忙等。
 * 由 usb_core 在轮询任务启动时调用 uhci_set_can_sleep(true) 打开。 */
static bool g_can_sleep = false;

void uhci_set_can_sleep(bool on)
{
    g_can_sleep = on;
}

static void usb_delay_ms(uint32_t ms)
{
    if (g_can_sleep) {
        msleep(ms);
    } else {
        usb_udelay(ms * 1000u);
    }
}

static uint32_t mk_token(uint32_t pid, uint8_t addr, uint8_t ep, int toggle,
                         uint16_t maxlen)
{
    uint32_t t = pid & 0xFFu;
    t |= ((uint32_t)(addr & 0x7Fu)) << 8;
    t |= ((uint32_t)(ep & 0xFu)) << 15;
    if (toggle) {
        t |= (1u << 19);
    }
    t |= ((uint32_t)(maxlen == 0 ? 0u : (uint32_t)(maxlen - 1))) << 21;
    return t;
}

/* 重建"直接 TD 调度链"：帧表项 -> 首个中断 TD -> ... -> 末 TD(终止)。
 * 不使用 QH：QEMU UHCI 对 QH 的处理与本实现约定不一致（QH 下 TD 从不执行），
 * 而帧表项直接指向 TD 链稳定可用（USB 1.1 规范允许 frame entry 指向 TD）。 */
static void uhci_rebuild_chain(void)
{
    uhci_td_t *first = NULL;
    uhci_td_t *prev = NULL;
    for (int i = 0; i < UHCI_MAX_INT_SLOTS; i++) {
        if (!g_int[i].active) {
            continue;
        }
        g_int[i].td->next = UHCI_PTR_TERM;
        if (!first) {
            first = g_int[i].td;
        } else {
            prev->next = (uint32_t)VIRT_TO_PHYS(g_int[i].td) | UHCI_PTR_DEPTH;
        }
        prev = g_int[i].td;
    }
    uint32_t head = first ? (uint32_t)VIRT_TO_PHYS(first) : UHCI_PTR_TERM;
    if (g_frame) {
        for (int i = 0; i < 1024; i++) {
            g_frame[i] = head;
        }
    }
}

/* 扫描 PCI 查找 UHCI 控制器（prog-if == 0x00）。 */
static bool uhci_find(pci_dev_t *out)
{
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint16_t vid = pci_cfg_read16((uint8_t)bus, dev, 0, PCI_CFG_VENDOR_ID);
            if (vid == 0xFFFF) {
                continue;
            }
            uint8_t nfunc = (pci_cfg_read8((uint8_t)bus, dev, 0, PCI_CFG_HEADER_TYPE)
                             & 0x80) ? 8 : 1;
            for (uint8_t fn = 0; fn < nfunc; fn++) {
                uint16_t v = pci_cfg_read16((uint8_t)bus, dev, fn, PCI_CFG_VENDOR_ID);
                if (v == 0xFFFF) {
                    continue;
                }
                uint8_t cc = pci_cfg_read8((uint8_t)bus, dev, fn, PCI_CFG_CLASS);
                uint8_t sc = pci_cfg_read8((uint8_t)bus, dev, fn, PCI_CFG_SUBCLASS);
                uint8_t pi = pci_cfg_read8((uint8_t)bus, dev, fn, PCI_CFG_PROG_IF);
                if (cc == 0x0C && sc == 0x03 && pi == 0x00) {   /* 0x00=UHCI */
                    out->bus = (uint8_t)bus;
                    out->dev = dev;
                    out->func = fn;
                    out->vendor_id = v;
                    out->device_id = pci_cfg_read16((uint8_t)bus, dev, fn,
                                                    PCI_CFG_DEVICE_ID);
                    return true;
                }
            }
        }
    }
    return false;
}

bool uhci_present(void)
{
    return g_ready;
}

bool uhci_init(void)
{
    if (g_ready) {
        return true;
    }
    pci_dev_t d;
    if (!uhci_find(&d)) {
        kprintf("[uhci] no UHCI controller found\n");
        return false;
    }
    kprintf("[uhci] found PCI %u:%u.%u vid=%04x did=%04x\n",
            (unsigned)d.bus, (unsigned)d.dev, (unsigned)d.func,
            d.vendor_id, d.device_id);

    /* 1) 禁用 BIOS legacy USB（按 OSDev：向 PCI 0xC0 写 0x2000） */
    pci_cfg_write16(d.bus, d.dev, d.func, 0xC0, 0x2000);

    /* 2) 使能 I/O 空间 + 总线主控（DMA） */
    uint16_t cmd = pci_cfg_read16(d.bus, d.dev, d.func, PCI_CFG_COMMAND);
    cmd |= PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER;
    pci_cfg_write16(d.bus, d.dev, d.func, PCI_CFG_COMMAND, cmd);

    /* 3) 扫描 BAR0..BAR5 找 I/O BAR（bit0=1），屏蔽低 2 位取 I/O 基址。
     * 注意：Intel PIIX3 UHCI 的 I/O 基址寄存器在 BAR4（offset 0x20），
     * 不可假定 BAR0。 */
    uint16_t io_base = 0;
    for (int bi = 0; bi < 6; bi++) {
        uint32_t bar = pci_cfg_read32(d.bus, d.dev, d.func,
                                      (uint8_t)(PCI_CFG_BAR0 + bi * 4));
        if (bar & 0x1u) {
            io_base = (uint16_t)(bar & ~0x3u);
            break;
        }
    }
    if (!io_base) {
        kprintf("[uhci] no I/O BAR found\n");
        return false;
    }
    g_io = io_base;

    /* 4) 复位：HCRESET（自清）+ GRESET */
    uw16(UHCI_USBCMD, UHCI_CMD_HCRESET);
    usb_udelay(20000);
    for (int i = 0; i < 100 && !(ur16(UHCI_USBSTS) & UHCI_STS_HCHALTED); i++) {
        usb_udelay(1000);
    }
    uw16(UHCI_USBCMD, UHCI_CMD_GRESET);
    usb_udelay(20000);
    uw16(UHCI_USBCMD, 0);
    usb_udelay(5000);

    /* 5) 帧列表（4KB 对齐，1024 项，初始全终止） */
    void *fl = pmm_alloc_page();
    if (!fl) {
        return false;
    }
    g_frame_phys = (uint64_t)fl;
    g_frame = (uint32_t *)PHYS_TO_VIRT(g_frame_phys);
    for (int i = 0; i < 1024; i++) {
        g_frame[i] = UHCI_PTR_TERM;
    }
    uw32(UHCI_FRBASEADD, (uint32_t)g_frame_phys);
    outb((uint16_t)(g_io + UHCI_SOFMOD), 0x40);
    uw16(UHCI_FRNUM, 0);

    /* 6) 控制结构页（QH@0，TDs@32，setup 紧随 TDs） */
    void *cs = pmm_alloc_page();
    void *dp = pmm_alloc_page();
    if (!cs || !dp) {
        if (cs) pmm_free_page(cs);
        if (dp) pmm_free_page(dp);
        return false;
    }
    g_ctrl_phys = (uint64_t)cs;
    g_ctrl = (uint8_t *)PHYS_TO_VIRT(g_ctrl_phys);
    g_data_phys = (uint64_t)dp;
    g_data = (uint8_t *)PHYS_TO_VIRT(g_data_phys);
    memset(g_ctrl, 0, 4096);
    memset(g_data, 0, 4096);
    g_td = (uhci_td_t *)(g_ctrl + 32);                     /* offset 32  */
    g_setup = g_ctrl + 32 + 32 * UHCI_MAX_CTRL_TD;         /* setup 包   */

    for (int i = 0; i < UHCI_MAX_INT_SLOTS; i++) {
        g_int[i].active = 0;
    }
    g_int_chain = UHCI_PTR_TERM;

    /* 7) 探测端口数：PORTSC bit7 恒为 1 且值 != 0xFFFF 即为有效端口 */
    g_ports = 0;
    for (int p = 1; p <= 8; p++) {
        uint16_t ps = ur16((uint16_t)(UHCI_PORTSC1 + 2 * (p - 1)));
        if (ps == 0xFFFF || !(ps & 0x0080)) {
            break;
        }
        g_ports = p;
    }

    /* 8) 轮询模式：先链接帧表，再启动控制器（RS|CF|MAXP） */
    uw16(UHCI_USBINTR, 0);
    uhci_rebuild_chain();
    uw16(UHCI_USBCMD, (uint16_t)(UHCI_CMD_RS | UHCI_CMD_CF | UHCI_CMD_MAXP));
    usb_udelay(10000);

    g_ready = true;
    kprintf("[uhci] ready: I/O=0x%X ports=%d PCI %u:%u.%u\n",
            (unsigned)g_io, g_ports, (unsigned)d.bus, (unsigned)d.dev,
            (unsigned)d.func);
    return true;
}

bool uhci_root_port_reset(int port, bool *low_speed)
{
    if (!g_ready || port < 1 || port > g_ports) {
        return false;
    }
    uint16_t reg = (uint16_t)(UHCI_PORTSC1 + 2 * (port - 1));
    uint16_t ps = ur16(reg);
    if (!(ps & UHCI_PORT_CCS)) {
        return false;
    }
    /* SetReset -> 100ms -> ClearReset -> 50ms -> SetPED（OSDev 时序）。
     * 这 100/50/30ms 用 usb_delay_ms：任务上下文走 msleep 让出 CPU（不再忙等）。 */
    uw16(reg, UHCI_PORT_RESET);
    usb_delay_ms(100);
    uint16_t cur = ur16(reg);
    uw16(reg, (uint16_t)(cur & ~UHCI_PORT_RESET));
    usb_delay_ms(50);

    ps = ur16(reg);
    if (!(ps & UHCI_PORT_PED)) {
        uw16(reg, (uint16_t)(ps | UHCI_PORT_PED));
    }
    for (int i = 0; i < 100; i++) {
        ps = ur16(reg);
        if (ps & UHCI_PORT_PED) {
            break;
        }
        usb_udelay(1000);
    }
    if (!(ps & UHCI_PORT_PED)) {
        return false;
    }
    usb_delay_ms(30);           /* 使能后稳定 30ms，再开始控制传输（可睡眠让出 CPU） */
    if (low_speed) {
        *low_speed = (ps & UHCI_PORT_LSDA) ? true : false;
    }
    return true;
}

/* 端点 0 控制传输（重试感知 NAK；带超时）。 */
int uhci_control(uint8_t addr, uint8_t low_speed, uint8_t mps0,
                 uint8_t bmRequestType, uint8_t bRequest,
                 uint16_t wValue, uint16_t wIndex, void *data, uint16_t len)
{
    if (!g_ready || len > 480) {
        return -1;
    }
    if (mps0 == 0) {
        mps0 = 8;
    }

    /* setup 包 */
    g_setup[0] = bmRequestType;
    g_setup[1] = bRequest;
    g_setup[2] = (uint8_t)(wValue & 0xFF);
    g_setup[3] = (uint8_t)(wValue >> 8);
    g_setup[4] = (uint8_t)(wIndex & 0xFF);
    g_setup[5] = (uint8_t)(wIndex >> 8);
    g_setup[6] = (uint8_t)(len & 0xFF);
    g_setup[7] = (uint8_t)(len >> 8);

    int dir_in = (bmRequestType & USB_DIR_IN) ? 1 : 0;
    uint32_t ls = low_speed ? UHCI_TD_LS : 0;
    int ntd = 0;

    /* 构建 TD 链（每次尝试都重建 status/token） */
    int ndata = 0;
    uint16_t done = 0;
    while (done < len) {
        uint16_t chunk = (uint16_t)(len - done);
        if (chunk > mps0) {
            chunk = mps0;
        }
        done = (uint16_t)(done + chunk);
        ndata++;
    }

    for (int attempt = 0; attempt < 50; attempt++) {
        ntd = 0;
        /* SETUP */
        uhci_td_t *s = &g_td[ntd++];
        memset(s, 0, sizeof(*s));
        s->status = UHCI_TD_ACTIVE | UHCI_TD_CERR_3 | ls;
        s->token = mk_token(UHCI_PID_SETUP, addr, 0, 0, 8);
        s->buffer = (uint32_t)VIRT_TO_PHYS(g_setup);
        /* DATA */
        done = 0;
        int di = 0;
        while (done < len) {
            uint16_t chunk = (uint16_t)(len - done);
            if (chunk > mps0) {
                chunk = mps0;
            }
            uhci_td_t *dt = &g_td[ntd++];
            memset(dt, 0, sizeof(*dt));
            dt->status = UHCI_TD_ACTIVE | UHCI_TD_CERR_3 | ls;
            int tog = (di % 2 == 0) ? 1 : 0;   /* 数据段从 DATA1 开始交替 */
            dt->token = mk_token(dir_in ? UHCI_PID_IN : UHCI_PID_OUT, addr, 0,
                                 tog, chunk);
            dt->buffer = (uint32_t)(VIRT_TO_PHYS(g_data) + done);
            if (done + chunk >= len) {
                dt->status |= UHCI_TD_SPD;     /* 末段短包检测 */
            }
            done = (uint16_t)(done + chunk);
            di++;
        }
        /* STATUS（方向与数据段相反，toggle=1，0 字节） */
        uhci_td_t *st = &g_td[ntd++];
        memset(st, 0, sizeof(*st));
        st->status = UHCI_TD_ACTIVE | UHCI_TD_CERR_3 | ls | UHCI_TD_IOC;
        st->token = mk_token(dir_in ? UHCI_PID_OUT : UHCI_PID_IN, addr, 0, 1, 0);
        st->buffer = (uint32_t)VIRT_TO_PHYS(g_data);
        /* 链接：深度优先链，末 TD 终止 */
        for (int i = 0; i < ntd - 1; i++) {
            g_td[i].next = (uint32_t)VIRT_TO_PHYS(&g_td[i + 1]) | UHCI_PTR_DEPTH;
        }
        g_td[ntd - 1].next = UHCI_PTR_TERM;

        /* 交给控制器执行：帧表项直接指向控制 TD 链（setup->data->status） */
        for (int i = 0; i < 1024; i++) {
            g_frame[i] = (uint32_t)VIRT_TO_PHYS(&g_td[0]);
        }

        int spins = 0;
        while ((g_td[ntd - 1].status & UHCI_TD_ACTIVE) && spins < 500) {
            usb_udelay(200);
            spins++;
        }
        uhci_rebuild_chain();       /* 恢复中断调度链 */

        /* 判定结果 */
        uint32_t last = g_td[ntd - 1].status;
        if (last & UHCI_TD_ACTIVE) {
            return -1;                          /* 超时 */
        }
        if (last & (UHCI_TD_STALLED | UHCI_TD_DBE | UHCI_TD_BABBLE)) {
            return -1;                          /* 硬错误 */
        }
        if (last & UHCI_TD_NAK) {
            usb_udelay(2000);
            continue;                           /* NAK：重试 */
        }
        /* setup 阶段错误判定 */
        if (g_td[0].status & (UHCI_TD_STALLED | UHCI_TD_CRC | UHCI_TD_BITSTUFF |
                              UHCI_TD_DBE | UHCI_TD_BABBLE)) {
            return -1;
        }
        /* 汇总数据段实际长度 */
        int actual = 0;
        int err = 0;
        for (int k = 0; k < ndata; k++) {
            uint32_t ds = g_td[1 + k].status;
            if (ds & (UHCI_TD_STALLED | UHCI_TD_CRC | UHCI_TD_BITSTUFF |
                      UHCI_TD_DBE | UHCI_TD_BABBLE)) {
                err = 1;
                break;
            }
            uint32_t fl = ds & UHCI_TD_ACTLEN_MASK;
            if (fl == UHCI_TD_ACTLEN_MASK) {
                err = 1;
                break;
            }
            actual += (int)(fl + 1);
        }
        if (err) {
            return -1;
        }
        if (actual > (int)len) {
            actual = (int)len;
        }
        if (dir_in && data && len > 0 && actual > 0) {
            memcpy(data, g_data, (size_t)actual);
        }
        return actual;
    }
    return -1;   /* 重试耗尽 */
}

int uhci_int_add(uint8_t addr, uint8_t low_speed, uint8_t ep,
                 uint16_t mps, uint8_t interval, void *buf, uint16_t len)
{
    (void)interval;
    if (!g_ready || len == 0 || len > 4000) {
        return -1;
    }
    int slot = -1;
    for (int i = 0; i < UHCI_MAX_INT_SLOTS; i++) {
        if (!g_int[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return -1;
    }
    void *pg = pmm_alloc_page();
    if (!pg) {
        return -1;
    }
    uhci_int_slot_t *s = &g_int[slot];
    memset(s, 0, sizeof(*s));
    s->dma_phys = (uint64_t)pg;
    s->dma_virt = (uint8_t *)PHYS_TO_VIRT(s->dma_phys);
    memset(s->dma_virt, 0, 4096);
    s->qh = (uhci_qh_t *)s->dma_virt;                 /* offset 0  */
    s->td = (uhci_td_t *)(s->dma_virt + 16);          /* offset 16 */
    s->data = s->dma_virt + 64;
    s->addr = addr;
    s->ep = (uint8_t)(ep & 0x0F);
    s->low_speed = low_speed;
    s->mps = (mps == 0) ? 8 : mps;
    s->len = len;
    s->buf_virt = (uint8_t *)buf;
    s->toggle = 0;

    s->td->next = UHCI_PTR_TERM;
    s->td->buffer = (uint32_t)VIRT_TO_PHYS(s->data);
    s->td->token = mk_token(UHCI_PID_IN, addr, s->ep, 0, s->mps);
    s->td->status = UHCI_TD_ACTIVE | UHCI_TD_CERR_3 | UHCI_TD_IOC |
                    (low_speed ? UHCI_TD_LS : 0);
    s->active = 1;

    uhci_rebuild_chain();
    return slot;
}

void uhci_int_remove(int slot)
{
    if (slot < 0 || slot >= UHCI_MAX_INT_SLOTS || !g_int[slot].active) {
        return;
    }
    uhci_int_slot_t *s = &g_int[slot];
    s->active = 0;
    if (s->dma_phys) {
        pmm_free_page((void *)s->dma_phys);
        s->dma_phys = 0;
    }
    uhci_rebuild_chain();
}

int uhci_int_poll(int slot)
{
    if (slot < 0 || slot >= UHCI_MAX_INT_SLOTS || !g_int[slot].active) {
        return -1;
    }
    uhci_int_slot_t *s = &g_int[slot];
    uhci_td_t *td = s->td;
    if (td->status & UHCI_TD_ACTIVE) {
        return 0;                                     /* 尚未完成 */
    }
    uint32_t stv = td->status;
    if (stv & UHCI_TD_NAK) {
        /* 无数据：重新激活，不翻转 toggle */
        td->status = UHCI_TD_ACTIVE | UHCI_TD_CERR_3 | UHCI_TD_IOC |
                     (s->low_speed ? UHCI_TD_LS : 0);
        return 0;
    }
    if (stv & (UHCI_TD_STALLED | UHCI_TD_CRC | UHCI_TD_BITSTUFF |
               UHCI_TD_DBE | UHCI_TD_BABBLE)) {
        uhci_int_remove(slot);                        /* 端点错误：停用 */
        return -1;
    }
    uint32_t fl = stv & UHCI_TD_ACTLEN_MASK;
    int actual = (fl == UHCI_TD_ACTLEN_MASK) ? 0 : (int)(fl + 1);
    if (actual > (int)s->len) {
        actual = (int)s->len;
    }
    if (actual > 0 && s->buf_virt) {
        memcpy(s->buf_virt, s->data, (size_t)actual);
    }
    /* 重新激活并翻转 toggle（UHCI 不自动翻转） */
    s->toggle ^= 1;
    td->token = mk_token(UHCI_PID_IN, s->addr, s->ep, s->toggle, s->mps);
    td->status = UHCI_TD_ACTIVE | UHCI_TD_CERR_3 | UHCI_TD_IOC |
                 (s->low_speed ? UHCI_TD_LS : 0);
    return actual;
}

/* 根端口轮询：处理连接变化（即插即用入口）。
 * 关键修复：除响应 CSC（连接变化）位外，对【已连接(CCS)但尚未枚举】的根端口也
 * 主动复位+枚举。否则设备若在内核 USB 轮询启动前就已连接（CSC 在控制器复位时
 * 已被清），将永远不被枚举 —— 表现为 USB 键鼠插上却完全无响应。每端口记录枚举
 * 状态，支持可靠的即插即用与热插拔（断开清状态、重连再枚举）。 */
static bool g_root_enumed[8];   /* 每根端口是否已枚举（避免重复枚举风暴） */
/* 枚举失败退避计数（单位：轮询轮次，每轮约 8ms）：>0 时本端口跳过本轮复位+枚举。
 * 关键：uhci_root_port_reset 内含 SetReset 100ms + ClearReset 50ms + 稳定 30ms 的
 * 忙等（约 180ms）。若某端口枚举持续失败而 g_root_enumed 保持 false，原实现会
 * 【每轮】都重做这 ~180ms 忙等，使 usb_service 任务几乎独占 CPU（表现为整机卡顿、
 * 鼠标延迟）。加入退避后，失败仅每 ~512ms 重试一次，杜绝该系统性空耗。 */
static uint8_t g_root_retry[8];

void uhci_poll(void)
{
    if (!g_ready) {
        return;
    }
    /* 清除总线错误（写 1 清） */
    uint16_t sts = ur16(UHCI_USBSTS);
    if (sts & (UHCI_STS_HSE | UHCI_STS_HCPE | UHCI_STS_ERROR)) {
        uw16(UHCI_USBSTS, (uint16_t)(sts & (UHCI_STS_HSE | UHCI_STS_HCPE |
                                            UHCI_STS_ERROR | UHCI_STS_USBINT)));
    }
    for (int p = 1; p <= g_ports; p++) {
        uint16_t reg = (uint16_t)(UHCI_PORTSC1 + 2 * (p - 1));
        uint16_t ps = ur16(reg);
        bool csc  = (ps & UHCI_PORT_CSC) ? true : false;
        bool ccs  = (ps & UHCI_PORT_CCS) ? true : false;
        if (csc) {
            uw16(reg, UHCI_PORT_CSC);                 /* 写 1 清变化位 */
        }
        if (ccs && !g_root_enumed[p]) {
            if (g_root_retry[p] > 0) {
                g_root_retry[p]--;      /* 退避中：本轮跳过（避免每轮 ~180ms 忙等） */
            } else {
                bool ls = false;
                if (uhci_root_port_reset(p, &ls)) {
                    g_port_ls[p] = ls ? 1 : 0;
                    int idx = usb_core_root_connect(p, ls);
                    if (idx >= 0) {
                        g_root_enumed[p] = true;
                    } else {
                        /* 枚举失败：保持未枚举但退避 ~512ms 后再试，绝不每轮重做复位 */
                        g_root_retry[p] = 64;
                        kprintf("[uhci] port %d enum failed, retry in ~512ms\n", p);
                    }
                } else {
                    g_root_retry[p] = 64;   /* 复位失败同样退避 */
                }
            }
        } else if (!ccs && g_root_enumed[p]) {
            usb_core_root_disconnect(p);
            g_root_enumed[p] = false;
            g_root_retry[p] = 0;
        }
    }
}
