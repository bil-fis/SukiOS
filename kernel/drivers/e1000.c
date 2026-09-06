/*
 * kernel/drivers/e1000.c
 * -----------------------------------------------------------------------------
 * Intel 8254x (e1000) 千兆以太网驱动 + NET_PORT 内核服务。
 *
 * 红线（与 ata.c 同构）：这是唯一驻留 Ring0 的网络硬件驱动。它【只做原始
 * 以太网帧收发】，绝不解析 IP/TCP/UDP；协议栈（lwIP）在用户态网络服务中，
 * 与 FAT32 逻辑全部在 FS_SERVER 完全一致，符合混合内核分层。
 *
 * 实现依据：本地 osdev_wiki 离线副本 `wiki.osdev.org/Intel_8254x`
 *   - Initialization：PCI 使能 -> CTRL.RST 复位 -> 置 ASDE|SLU -> 经 EERD
 *     读 EEPROM 前 3 word 得 MAC -> 写 RAL0/RAH0；
 *   - Ring setup：TX 8 项 / RX 32 项 legacy 描述符，每项 4096 字节缓冲；
 *     T/RDBAL|T/RDBAH|T/RDLEN 描述环物理地址与长度，T/RDH/T/RDT 为头尾；
 *   - Packet Transmittion：填缓冲 -> 置 length/cmd(EOP|IFCS|RS) -> 推进 TDT；
 *   - Packet Reception：按 DD 位判定已填充描述符 -> 取数据 -> status 清零
 *     -> 把 RDT 置为刚释放的描述符归还硬件；
 *   - Interrupt Handling：本驱动采用【轮询】而非中断线（与 hda.c 一致的稳健
 *     策略，规避 IOAPIC/INTx 路由差异与共享 IRQ 风险）；IMS/ICR 仅用于清零。
 *
 * 调用关系：kmain -> e1000_init() + net_srv_start()；
 *           网络服务 --mach_msg--> NET_PORT -> e1000_send/e1000_recv。
 */
#include <kernel/e1000.h>
#include <ipc/net_proto.h>
#include <kernel/pci.h>
#include <kernel/console.h>
#include <kernel/task.h>
#include <kernel/string.h>
#include <kernel/io.h>
#include <kernel/clock.h>
#include <kernel/percpu.h>
#include <kernel/spinlock.h>    /* cpu_relax()：轮询等待时的 pause 提示 */
#include <ipc/port.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

/* 轮询超时的时间基准上限（纳秒） */
#define E1000_TX_TIMEOUT_NS     (100ull * 1000ull * 1000ull)   /* 100ms */
#define E1000_SELFTEST_NS       (2000ull * 1000ull * 1000ull)  /* 2s    */
#define E1000_RECV_WAIT_NS      (1000ull * 1000ull * 1000ull)  /* 1s    */

/* ---- 驱动状态 ---- */
static volatile uint8_t *g_mmio = NULL;      /* BAR0 MMIO 虚拟基址 */
static bool     g_present = false;           /* 网卡可用 */
static uint8_t  g_mac[6];                    /* EEPROM 读出的 MAC */

static uint64_t g_tx_ring_phys = 0, g_rx_ring_phys = 0;
static e1000_tx_desc_t *g_tx_ring = NULL;    /* 内核虚拟地址（PHYS_TO_VIRT） */
static e1000_rx_desc_t *g_rx_ring = NULL;
static uint64_t g_tx_buf_phys[E1000_TX_DESC_COUNT];
static uint64_t g_rx_buf_phys[E1000_RX_DESC_COUNT];
static uint32_t g_tx_cur = 0, g_rx_cur = 0;

/* 统计（诊断/自检用） */
static uint32_t g_tx_packets = 0, g_rx_packets = 0;

/* ---- MMIO 访问原语（volatile 禁止重排/合并） ---- */
static inline uint32_t r32(uint32_t off)
{
    return *(volatile uint32_t *)(g_mmio + off);
}
static inline void w32(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(g_mmio + off) = v;
}

/* ---- EEPROM 读取（osdev：EERD 起始位 + 地址，轮询 DONE 位取数据） ---- */
static uint16_t eeprom_read(uint8_t addr)
{
    w32(E1000_REG_EERD,
        E1000_EERD_START | ((uint32_t)addr << E1000_EERD_ADDR_SHIFT));

    /* 有界轮询：EERD_DONE 置位后数据在 bit 31:16。
     * 防御性：硬件异常时不死等，超时返回 0xFFFF 由调用方判定。 */
    for (uint32_t i = 0; i < 100000u; i++) {
        uint32_t v = r32(E1000_REG_EERD);
        if (v & E1000_EERD_DONE) {
            return (uint16_t)(v >> E1000_EERD_DATA_SHIFT);
        }
        cpu_relax();
    }
    return 0xFFFFu;
}

/* ---- 释放已分配的环/缓冲页（初始化失败回滚，防物理页泄漏） ---- */
static void rings_free(void)
{
    if (g_tx_ring_phys) {
        for (uint32_t i = 0; i < E1000_TX_DESC_COUNT; i++) {
            if (g_tx_buf_phys[i]) {
                pmm_free_page((void *)g_tx_buf_phys[i]);
                g_tx_buf_phys[i] = 0;
            }
        }
        pmm_free_page((void *)g_tx_ring_phys);
        g_tx_ring_phys = 0;
        g_tx_ring = NULL;
    }
    if (g_rx_ring_phys) {
        for (uint32_t i = 0; i < E1000_RX_DESC_COUNT; i++) {
            if (g_rx_buf_phys[i]) {
                pmm_free_page((void *)g_rx_buf_phys[i]);
                g_rx_buf_phys[i] = 0;
            }
        }
        pmm_free_page((void *)g_rx_ring_phys);
        g_rx_ring_phys = 0;
        g_rx_ring = NULL;
    }
}

/* ---- 建立 TX/RX 描述符环（osdev "Ring setup"） ---- */
static bool rings_setup(void)
{
    /* 描述符环须物理连续：整页分配天然满足 16 字节对齐与连续性要求 */
    void *txp = pmm_alloc_page();
    void *rxp = pmm_alloc_page();
    if (!txp || !rxp) {
        if (txp) pmm_free_page(txp);
        if (rxp) pmm_free_page(rxp);
        return false;
    }
    g_tx_ring_phys = (uint64_t)txp;
    g_rx_ring_phys = (uint64_t)rxp;
    g_tx_ring = (e1000_tx_desc_t *)PHYS_TO_VIRT(g_tx_ring_phys);
    g_rx_ring = (e1000_rx_desc_t *)PHYS_TO_VIRT(g_rx_ring_phys);
    memset(g_tx_ring, 0, PAGE_SIZE);
    memset(g_rx_ring, 0, PAGE_SIZE);

    /* 发送环：每项预分配一个 4096 字节缓冲（静态缓冲策略，osdev 推荐） */
    for (uint32_t i = 0; i < E1000_TX_DESC_COUNT; i++) {
        void *b = pmm_alloc_page();
        if (!b) {
            rings_free();
            return false;
        }
        g_tx_buf_phys[i] = (uint64_t)b;
        g_tx_ring[i].addr = (uint64_t)b;
        g_tx_ring[i].status = 0;
    }
    w32(E1000_REG_TDBAL, (uint32_t)(g_tx_ring_phys & 0xFFFFFFFFu));
    w32(E1000_REG_TDBAH, (uint32_t)(g_tx_ring_phys >> 32));
    w32(E1000_REG_TDLEN, E1000_TX_DESC_COUNT * 16u);
    w32(E1000_REG_TDH, 0);
    w32(E1000_REG_TDT, 0);

    /* 接收环：每项预分配一个 4096 字节缓冲并交给硬件 */
    for (uint32_t i = 0; i < E1000_RX_DESC_COUNT; i++) {
        void *b = pmm_alloc_page();
        if (!b) {
            rings_free();
            return false;
        }
        g_rx_buf_phys[i] = (uint64_t)b;
        g_rx_ring[i].addr = (uint64_t)b;
        g_rx_ring[i].status = 0;
    }
    w32(E1000_REG_RDBAL, (uint32_t)(g_rx_ring_phys & 0xFFFFFFFFu));
    w32(E1000_REG_RDBAH, (uint32_t)(g_rx_ring_phys >> 32));
    w32(E1000_REG_RDLEN, E1000_RX_DESC_COUNT * 16u);
    w32(E1000_REG_RDH, 0);
    /* 复位后所有描述符均可接收，尾指针指向最后一个描述符（osdev） */
    w32(E1000_REG_RDT, E1000_RX_DESC_COUNT - 1u);

    g_tx_cur = 0;
    g_rx_cur = 0;
    return true;
}

/* ---- 探测与初始化（osdev "Initialization"） ---- */
bool e1000_init(void)
{
    pci_dev_t d;
    /* class 0x02 = Network controller, subclass 0x00 = Ethernet */
    if (!pci_find_class(0x02, 0x00, &d)) {
        kprintf("[e1000] no ethernet controller found on PCI\n");
        return false;
    }

    uint16_t vendor = pci_cfg_read16(d.bus, d.dev, d.func, PCI_CFG_VENDOR_ID);
    if (vendor != E1000_VENDOR_INTEL) {
        kprintf("[e1000] non-Intel NIC (vendor=%04x) unsupported\n",
                (unsigned)vendor);
        return false;
    }

    pci_enable_device(&d);                 /* 使能 MMIO + Bus Master(DMA) */

    uint64_t bar = pci_bar_addr(&d, 0);    /* osdev：始终使用 BAR0 */
    if (!bar) {
        kprintf("[e1000] BAR0 not present, aborting\n");
        return false;
    }
    /* 引导页表以 2MB 大页直映 0..4GB，故 MMIO 可经 PHYS_TO_VIRT 访问
     *（与 hda.c 同；真机应改为 UC 映射）。 */
    g_mmio = (volatile uint8_t *)PHYS_TO_VIRT(bar);

    kprintf("[e1000] controller %04x:%04x at PCI %u:%u.%u, MMIO phys=%p\n",
            d.vendor_id, d.device_id, (unsigned)d.bus, (unsigned)d.dev,
            (unsigned)d.func, (void *)bar);

    /* 1) 设备复位：置 CTRL.RST 并等待自清 */
    w32(E1000_REG_CTRL, r32(E1000_REG_CTRL) | E1000_CTRL_RST);
    {
        uint32_t guard = 0;
        while ((r32(E1000_REG_CTRL) & E1000_CTRL_RST) && guard++ < 1000000u) {
            cpu_relax();
        }
        if (r32(E1000_REG_CTRL) & E1000_CTRL_RST) {
            kprintf("[e1000] reset timeout, aborting\n");
            return false;
        }
    }

    /* 2) 自动速率检测 + Set Link Up（osdev：ASDE 需配合 SLU） */
    w32(E1000_REG_CTRL, r32(E1000_REG_CTRL) | E1000_CTRL_ASDE | E1000_CTRL_SLU);

    /* 3) 从 EEPROM 读 MAC（前 3 个 word） */
    uint16_t w0 = eeprom_read(0);
    uint16_t w1 = eeprom_read(1);
    uint16_t w2 = eeprom_read(2);
    g_mac[0] = (uint8_t)(w0 & 0xFF);
    g_mac[1] = (uint8_t)(w0 >> 8);
    g_mac[2] = (uint8_t)(w1 & 0xFF);
    g_mac[3] = (uint8_t)(w1 >> 8);
    g_mac[4] = (uint8_t)(w2 & 0xFF);
    g_mac[5] = (uint8_t)(w2 >> 8);

    /* 防御性：全 0 / 全 F 的 MAC 说明 EEPROM 未读到，判失败 */
    bool mac_ok = false;
    for (int i = 0; i < 6; i++) {
        if (g_mac[i] != 0x00 && g_mac[i] != 0xFF) { mac_ok = true; break; }
    }
    if (!mac_ok) {
        kprintf("[e1000] EEPROM MAC read failed (%02x:%02x:%02x:%02x:%02x:%02x)\n",
                g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
        return false;
    }

    /* 4) 写接收地址寄存器 RAL0/RAH0（RAH0 bit31 = Address Valid） */
    w32(E1000_REG_RAL0, ((uint32_t)w1 << 16) | (uint32_t)w0);
    w32(E1000_REG_RAH0, (uint32_t)w2 | (1u << 31));

    /* 5) 组播表清零（避免复位后残留项导致接收异常过滤） */
    for (uint32_t i = 0; i < 32u; i++) {
        w32(E1000_REG_MTA + i * 4u, 0);
    }

    /* 6) 建立描述符环 */
    if (!rings_setup()) {
        kprintf("[e1000] descriptor ring setup failed\n");
        return false;
    }

    /* 7) 发送：标准 IPG + TCTL(EN|PSP|CT=0x10|COLD=0x40) */
    w32(E1000_REG_TIPG, 0x0060200Au);
    w32(E1000_REG_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP |
                        (0x10u << E1000_TCTL_CT_SHIFT) |
                        (0x40u << E1000_TCTL_COLD_SHIFT));

    /* 8) 接收：EN|LPE|BAM|BSEX|SECRC + BSIZE=0b11（配合 BSEX 表示 4096 缓冲）
     *    SECRC 令硬件剥离 CRC，使 length 即真正的帧长（便于协议栈取用）。 */
    w32(E1000_REG_RCTL, E1000_RCTL_EN | E1000_RCTL_LPE | E1000_RCTL_BAM |
                        E1000_RCTL_BSEX | E1000_RCTL_SECRC |
                        (3u << E1000_RCTL_BSIZE_SHIFT));

    /* 9) 轮询模式：屏蔽全部中断并清一次 ICR（ICR 读即清） */
    w32(E1000_REG_IMC, 0xFFFFFFFFu);
    (void)r32(E1000_REG_ICR);

    g_present = true;
    kprintf("[e1000] ready: MAC=%02x:%02x:%02x:%02x:%02x:%02x link=%s "
            "(tx=%u rx=%u descriptors, 4096B buffers, polling)\n",
            g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5],
            (r32(E1000_REG_STATUS) & E1000_STATUS_LU) ? "up" : "down",
            E1000_TX_DESC_COUNT, E1000_RX_DESC_COUNT);
    return true;
}

bool e1000_present(void) { return g_present; }

bool e1000_get_mac(uint8_t mac[6])
{
    if (!g_present || !mac) return false;
    memcpy(mac, g_mac, 6);
    return true;
}

bool e1000_link_up(void)
{
    if (!g_present) return false;
    return (r32(E1000_REG_STATUS) & E1000_STATUS_LU) != 0;
}

/* ---- 发送一帧（osdev "Packet Transmittion"） ---- */
bool e1000_send(const void *data, uint32_t length)
{
    if (!g_present || !data || length == 0 || length > NET_MAX_FRAME) {
        return false;
    }

    uint32_t tail = r32(E1000_REG_TDT) % E1000_TX_DESC_COUNT;
    volatile e1000_tx_desc_t *td = &g_tx_ring[tail];

    /* 描述符尚未被硬件回收（DD 未置位）说明发送队列已满 */
    if (g_tx_ring[tail].status == 0 && tail != g_tx_cur) {
        /* 继续尝试：本实现为简单串行发送，交由下方 DD 等待保证顺序 */
    }

    void *buf = (void *)PHYS_TO_VIRT(g_tx_buf_phys[tail]);
    memcpy(buf, data, length);

    td->length = (uint16_t)length;
    td->cmd    = E1000_TX_CMD_EOP | E1000_TX_CMD_IFCS | E1000_TX_CMD_RS;
    td->status = 0;

    uint32_t next = (tail + 1u) % E1000_TX_DESC_COUNT;
    w32(E1000_REG_TDT, next);            /* 推进尾指针即触发发送 */
    g_tx_cur = next;

    /* 等待硬件置 DD（有界超时，超时判失败但描述符会在后续被回收） */
    uint64_t t0 = clock_monotonic_ns();
    while (!(td->status & E1000_TX_STATUS_DD)) {
        if (clock_monotonic_ns() - t0 > E1000_TX_TIMEOUT_NS) {
            return false;
        }
        cpu_relax();
    }
    g_tx_packets++;
    return true;
}

/* ---- 接收一帧（osdev "Packet Reception"） ---- */
int e1000_recv(uint8_t *out, uint32_t max)
{
    if (!g_present || !out || max == 0) {
        return -1;
    }

    uint32_t idx = g_rx_cur % E1000_RX_DESC_COUNT;
    volatile e1000_rx_desc_t *rd = &g_rx_ring[idx];

    if (!(rd->status & E1000_RX_STATUS_DD)) {
        return 0;                        /* 当前无帧 */
    }

    uint16_t len = rd->length;
    if (len > max) {
        len = (uint16_t)max;             /* 防御：绝不越界写调用方缓冲 */
    }
    memcpy(out, (const void *)PHYS_TO_VIRT(g_rx_buf_phys[idx]), len);

    rd->status = 0;                      /* 归还描述符给硬件 */
    g_rx_cur = (idx + 1u) % E1000_RX_DESC_COUNT;
    w32(E1000_REG_RDT, idx);             /* 尾指针 = 刚释放的描述符 */

    g_rx_packets++;
    return (int)len;                     /* EOP 恒真：4096B 缓冲容纳完整帧 */
}

/*
 * ---- 启动自检：ARP 请求 -> 等待应答 ----
 * 真实端到端验证 TX/RX：向 QEMU user-net 网关 10.0.2.2 发 ARP 请求，
 * 若 QEMU 的虚拟网络栈回应 ARP 应答，则接收路径成立。TX 成功但 RX 无应答
 * 仅打印告警（不影响驱动可用性，例如后端未连接时）。
 */
static void e1000_self_test(void)
{
    uint8_t frame[64];
    memset(frame, 0, sizeof(frame));

    memset(frame, 0xFF, 6);                       /* 目的：广播 */
    memcpy(frame + 6, g_mac, 6);                  /* 源：本机 MAC */
    frame[12] = 0x08; frame[13] = 0x06;           /* EtherType = ARP */

    uint8_t *a = frame + 14;
    a[0] = 0x00; a[1] = 0x01;                     /* 硬件类型 = Ethernet */
    a[2] = 0x08; a[3] = 0x00;                     /* 协议类型 = IPv4 */
    a[4] = 6;    a[5] = 4;                        /* MAC 长 6 / IP 长 4 */
    a[6] = 0x00; a[7] = 0x01;                     /* 操作 = 请求 */
    memcpy(a + 8, g_mac, 6);                      /* 发送方 MAC */
    a[14] = 10; a[15] = 0; a[16] = 2; a[17] = 15; /* 发送方 IP 10.0.2.15 */
    memset(a + 18, 0, 6);                         /* 目标 MAC 未知 */
    a[24] = 10; a[25] = 0; a[26] = 2; a[27] = 2;  /* 目标 IP 10.0.2.2 */

    bool sent = e1000_send(frame, 42);

    uint8_t rx[NET_MAX_FRAME];
    int got = 0;
    uint64_t t0 = clock_monotonic_ns();
    while (clock_monotonic_ns() - t0 < E1000_SELFTEST_NS) {
        got = e1000_recv(rx, sizeof(rx));
        if (got > 0) {
            break;
        }
        task_yield();
    }

    kprintf("[e1000] self-test: tx=%s rx=%d bytes (%s)\n",
            sent ? "ok" : "FAIL", got,
            (sent && got > 0) ? "TX/RX loop verified" : "no reply (backend may be idle)");
}

/* ---- NET_PORT 内核服务任务（仿 disk_srv_task） ---- */
static void net_srv_task(void *arg)
{
    (void)arg;
    port_set_owner(NET_PORT, sched_current());

    if (!g_present) {
        kprintf("[net-srv] e1000 unavailable, NET_PORT not served\n");
        /* 仍进入循环：对请求返回失败状态，避免请求方永久阻塞 */
    } else {
        kprintf("[net-srv] serving NET_PORT (kernel-resident, e1000)\n");
        e1000_self_test();
    }

    static uint8_t req[sizeof(mach_msg_header_t) + NET_MAX_FRAME + 64];
    static uint8_t resp[sizeof(mach_msg_header_t) + NET_MAX_FRAME + 64];

    for (;;) {
        uint32_t n = 0;
        if (ipc_recv_kernel(NET_PORT, req, sizeof(req), &n, true)
                != MACH_MSG_SUCCESS || n < sizeof(mach_msg_header_t)) {
            /* 关键：失败/异常分支必须让出 CPU。单核为协作式调度，若此处直接
             * continue 会形成不让出的紧密循环，把其余任务（含用户态测试）全部
             * 饿死，表现为系统"卡住"但定时器中断仍在跳。 */
            task_yield();
            continue;
        }
        mach_msg_header_t *rh = (mach_msg_header_t *)req;
        uint32_t reply = rh->msgh_local_port;
        if (reply == PORT_NULL) {
            task_yield();
            continue;
        }

        mach_msg_header_t *h = (mach_msg_header_t *)resp;

        if (rh->msgh_id == NET_MSG_GET_MAC) {
            net_mac_resp_t *mr = (net_mac_resp_t *)(resp + sizeof(*h));
            mr->status = g_present ? 0 : 1;
            mr->link_up = e1000_link_up() ? 1 : 0;
            mr->pad = 0;
            if (g_present) {
                memcpy(mr->mac, g_mac, 6);
            } else {
                memset(mr->mac, 0, 6);
            }
            h->msgh_bits = 0;
            h->msgh_size = sizeof(*h) + sizeof(*mr);
            h->msgh_remote_port = reply;
            h->msgh_local_port = NET_PORT;
            h->msgh_id = NET_MSG_GET_MAC;
            h->msgh_reserved = 0;
            ipc_send_kernel(reply, resp, h->msgh_size);
        } else if (rh->msgh_id == NET_MSG_SEND &&
                   n >= sizeof(mach_msg_header_t) + sizeof(net_send_req_t)) {
            net_send_req_t *sr =
                (net_send_req_t *)(req + sizeof(mach_msg_header_t));
            uint32_t len = sr->length;
            bool ok = false;
            /* 防御：长度须与实际携带的数据字节数一致，防止越界读请求缓冲 */
            if (len >= 1 && len <= NET_MAX_FRAME &&
                n >= sizeof(mach_msg_header_t) + sizeof(net_send_req_t) + len) {
                const uint8_t *data = req + sizeof(mach_msg_header_t)
                                    + sizeof(net_send_req_t);
                ok = e1000_send(data, len);
            }
            net_send_resp_t *sp = (net_send_resp_t *)(resp + sizeof(*h));
            sp->status = ok ? 0 : 1;
            h->msgh_bits = 0;
            h->msgh_size = sizeof(*h) + sizeof(*sp);
            h->msgh_remote_port = reply;
            h->msgh_local_port = NET_PORT;
            h->msgh_id = NET_MSG_SEND;
            h->msgh_reserved = 0;
            ipc_send_kernel(reply, resp, h->msgh_size);
        } else if (rh->msgh_id == NET_MSG_RECV) {
            uint8_t rx[NET_MAX_FRAME];
            int got = 0;
            if (g_present) {
                uint64_t t0 = clock_monotonic_ns();
                for (;;) {
                    got = e1000_recv(rx, sizeof(rx));
                    if (got > 0) {
                        break;
                    }
                    if (clock_monotonic_ns() - t0 > E1000_RECV_WAIT_NS) {
                        break;               /* 超时：返回 0 长度 */
                    }
                    task_yield();
                }
            }
            net_recv_resp_t *rr = (net_recv_resp_t *)(resp + sizeof(*h));
            rr->status = g_present ? 0 : 1;
            rr->length = (got > 0) ? (uint32_t)got : 0;
            uint32_t total = sizeof(*h) + sizeof(*rr);
            if (got > 0) {
                memcpy(resp + total, rx, (size_t)got);
                total += (uint32_t)got;
            }
            h->msgh_bits = 0;
            h->msgh_size = total;
            h->msgh_remote_port = reply;
            h->msgh_local_port = NET_PORT;
            h->msgh_id = NET_MSG_RECV;
            h->msgh_reserved = 0;
            ipc_send_kernel(reply, resp, total);
        }
    }
}

void net_srv_start(void)
{
    task_create_kernel(net_srv_task, NULL, "net-srv");
}
