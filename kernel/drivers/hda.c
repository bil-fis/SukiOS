/*
 * kernel/drivers/hda.c
 * -----------------------------------------------------------------------------
 * Intel High Definition Audio (HDA) 驱动。
 *
 * 依据：Intel HDA Specification Rev 1.0a（high-definition-audio-specification.pdf，
 *       CORB/RIRB §4.4.1，流描述符/BDL §3.3.35+，编解码器 verb §7.3.3/§7.3.4）
 *       与 wiki.osdev.org/Intel_High_Definition_Audio。
 * 注：寄存器布局遵循 QEMU intel-hda / osdev 采用的简化模型（流描述符每项 0x20
 *     字节，byte0=CTL0/RUN/SRST、byte2[7:4]=流号、0x5A=RINTCNT 等），与严格
 *     1.0a 的 SDnCTL 位布局略有差异；本驱动以 QEMU 验证为准，verb 编码数值与
 *     规范完全一致。
 *
 * 结构：
 *   1. PCI 探测：class 0x04 (Multimedia) / subclass 0x03 (Audio Device)。
 *      QEMU: -device intel-hda (8086:2668, ICH6)。BAR0 = 16KB MMIO 寄存器。
 *   2. 控制器复位（GCTL.CRST 0->1），读 STATESTS 得到编解码器位图。
 *   3. CORB/RIRB DMA 环初始化（各 1 物理页，256 项），所有 verb 走
 *      CORB/RIRB（spec 推荐路径；不依赖可选的立即命令接口）。
 *   4. 编解码器枚举：root -> AFG(功能组类型 0x01) -> 遍历 widget，
 *      找 Audio Output(DAC, type 0x0) 与输出能力 Pin Complex(type 0x4)，
 *      设置连接选择、功耗 D0、放大器 0dB 解静音、Pin OUT_EN、EAPD。
 *   5. 输出流：使用第一个 Output Stream 引擎（索引 = GCAP.ISS，流标签 1），
 *      BDL 32 项 × 4KB 物理页 = 128KB 循环缓冲，CBL=131072，LVI=31。
 *   6. 播放：Ring3 经 SYS_AUDIO_WRITE 拷入 PCM；驱动跟踪 LPIB（链路位置）
 *      计算已消费字节；预填充 48KB 后置 SDnCTL.RUN 启动 DMA。轮询模式，
 *      不使用中断（INTCTL=0），无 IRQ 处理器。
 *
 * 注意（真机移植）：MMIO 区域按引导页表（0..4GB 2MB 大页，WB 可缓存）
 * 经 PHYS_TO_VIRT 访问。QEMU 下正确；真机须改为 UC (PCD) 映射。
 *
 * 调用关系：kmain -> hda_init()；syscall.c(sys_audio_*) -> hda_pcm_*。
 */
#include <kernel/hda.h>
#include <kernel/pci.h>
#include <kernel/io.h>
#include <kernel/console.h>
#include <kernel/string.h>
#include <kernel/task.h>          /* sched_current()：H6 所有权校验 */
#include <kernel/spinlock.h>      /* P0-R1：SMP 下 PCM 状态机需自旋锁串行化 */
#include <mm/pmm.h>

/* M11 修复：hda_verb_raw 的忙等循环中将周期性让出 CPU（见下方实现） */
extern void task_yield(void);

/* ---- 控制器寄存器偏移（HDA spec §3.3） ---- */
#define REG_GCAP        0x00    /* u16: OSS[15:12] ISS[11:8] BSS[7:3] */
#define REG_VMIN        0x02
#define REG_VMAJ        0x03
#define REG_GCTL        0x08    /* u32: bit0 CRST */
#define REG_WAKEEN      0x0C
#define REG_STATESTS    0x0E    /* u16: 编解码器存在位图（写 1 清零） */
#define REG_INTCTL      0x20    /* u32: 全局/流中断使能（轮询模式=0） */
#define REG_CORBLBASE   0x40
#define REG_CORBUBASE   0x44
#define REG_CORBWP      0x48    /* u16: [7:0] 写指针 */
#define REG_CORBRP      0x4A    /* u16: bit15 RP 复位, [7:0] 读指针 */
#define REG_CORBCTL     0x4C    /* u8 : bit1 CORBRUN */
#define REG_CORBSIZE    0x4E    /* u8 : [1:0] 0=2,1=16,2=256 项 */
#define REG_RIRBLBASE   0x50
#define REG_RIRBUBASE   0x54
#define REG_RIRBWP      0x58    /* u16: bit15 WP 复位(只写), [7:0] 写指针 */
/* 0x5A 寄存器在真实 HDA 规范里是 RIRBRP（RIRB 读指针），但 QEMU 的
 * intel-hda 实现把它建模为 RINTCNT（rirb_cnt，响应中断计数）。QEMU 的
 * intel_hda_corb_run 有 `if (rirb_count == rirb_cnt) return;` 的闸门：
 * 默认 rirb_cnt=0 时 CORB 引擎根本不运行（即使 CORBRUN 已置位）。因此
 * 在 QEMU 下必须把 0x5A 写成非零值（本驱动用 0xFF=255，足够覆盖整个
 * 枚举/打开过程的 verb 数）才能让控制器消费 CORB 命令。真机需改为复位
 * RIRBRP(0x5A bit15) 并把 RINTCNT 设在 RIRBCTL[3:0]，此处以 QEMU 验证为准。 */
#define REG_RINTCNT     0x5A
#define REG_RIRBCTL     0x5C    /* u8 : bit1 RIRBDMAEN */
#define REG_RIRBSTS     0x5D
#define REG_RIRBSIZE    0x5E
#define REG_SD_BASE     0x80    /* 流描述符数组基址，每个 0x20 字节 */

/* 流描述符寄存器（相对流基址） */
#define SD_CTL0         0x00    /* u8 : bit0 SRST, bit1 RUN */
#define SD_CTL2         0x02    /* u8 : [7:4] 流标签 */
#define SD_STS          0x03
#define SD_LPIB         0x04    /* u32: 链路位置（环内字节偏移） */
#define SD_CBL          0x08    /* u32: 循环缓冲总长 */
#define SD_LVI          0x0C    /* u16: 最后有效 BDL 索引 */
#define SD_FMT          0x12    /* u16: 流格式 */
#define SD_BDPL         0x18
#define SD_BDPU         0x1C

/* ---- 编解码器 verb（HDA spec §7.3.3） ---- */
#define VERB_GET_PARAM      0xF00
#define VERB_GET_CONN_LIST  0xF02
#define VERB_SET_CONN_SEL   0x701
#define VERB_SET_POWER      0x705
#define VERB_SET_STREAM_CH  0x706
#define VERB_SET_PIN_CTL    0x707
#define VERB_SET_EAPD       0x70C
#define VERB_GET_CFG_DEF    0xF1C
/* 4-bit 长载荷 verb */
#define VERB4_SET_FORMAT    0x2     /* 16 位载荷 = 流格式 */
#define VERB4_SET_AMP       0x3     /* 16 位载荷 = 放大器设置 */

/* GET_PARAM 参数 id（§7.3.4） */
#define PAR_VENDOR_ID       0x00
#define PAR_SUB_NODE_CNT    0x04
#define PAR_FG_TYPE         0x05    /* [7:0]&0x7F: 0x01 = AFG */
#define PAR_AW_CAPS         0x09    /* [23:20] widget 类型 */
#define PAR_PIN_CAPS        0x0C    /* bit4 输出能力 */
#define PAR_CONN_LIST_LEN   0x0E
#define PAR_OUT_AMP_CAPS    0x12    /* [6:0] 0dB 偏移步数 */

#define WTYPE_DAC           0x0
#define WTYPE_PIN           0x4

/* ---- 驱动状态 ---- */
static volatile uint8_t *g_mmio = NULL;     /* BAR0 MMIO 虚拟基址 */
static bool     g_present = false;
static uint32_t g_cad = 0;                  /* 编解码器地址 */
static uint32_t g_nid_dac = 0, g_nid_pin = 0, g_nid_afg = 0;
static uint32_t g_oss_base_idx = 0;         /* 第一个输出流引擎索引 = ISS 数 */

static uint64_t g_corb_phys = 0, g_rirb_phys = 0, g_bdl_phys = 0;
static uint64_t g_buf_phys[HDA_BDL_ENTRIES];
static uint32_t g_corb_entries = 256, g_rirb_entries = 256;
static uint32_t g_rirb_rp = 0;              /* 软件侧 RIRB 读指针 */

/* 播放环状态（单打开者；由 syscall 层做所有权控制） */
static bool     g_opened = false;
static bool     g_running = false;
static uint64_t g_wr_ofs = 0;               /* 环内写偏移（< HDA_BUF_BYTES） */
/* H6 修复：记录当前打开输出流的任务（owner）。HDA 为全局单例资源，
 * 在引入本字段前任何任务都能随时 open 重置 DMA / 覆盖 BDL，破坏他人
 * 正在播放的流。现在 open 时登记 owner，非 owner 的后续 open/write/
 * queued/stop 一律拒绝，把"音频资源"纳入与端口同款的 owner 能力体系。 */
static task_t  *g_owner  = NULL;

/* P0-R1(H8)：PCM 状态机（g_opened/g_running/g_wr_ofs/g_owner + MMIO 流寄存器）
 * 的 SMP 串行化锁。单核时代靠 syscall 串行天然互斥；对称多核后两个核可能
 * 同时进入 hda_pcm_write（owner 任务 + 其 fork 子进程等），必须加锁。 */
static spinlock_t g_hda_lock = SPINLOCK_INIT("hda");

/* ---- MMIO 访问原语（非特权指令，内联即可；volatile 禁止重排/合并） ---- */
static inline uint32_t r32(uint32_t off) { return *(volatile uint32_t *)(g_mmio + off); }
static inline uint16_t r16(uint32_t off) { return *(volatile uint16_t *)(g_mmio + off); }
static inline uint8_t  r8(uint32_t off)  { return *(volatile uint8_t  *)(g_mmio + off); }
static inline void w32(uint32_t off, uint32_t v) { *(volatile uint32_t *)(g_mmio + off) = v; }
static inline void w16(uint32_t off, uint16_t v) { *(volatile uint16_t *)(g_mmio + off) = v; }
static inline void w8(uint32_t off, uint8_t v)   { *(volatile uint8_t  *)(g_mmio + off) = v; }

/* 微延迟：每次 io_wait 约 1us（0x80 端口写） */
static void udelay(uint32_t us)
{
    while (us--) {
        io_wait();
    }
}

/* ---- 控制器复位（spec §4.2.2：CRST 0->1，等待编解码器自枚举 >521us） ---- */
static bool hda_controller_reset(void)
{
    /* 进入复位（CRST=0） */
    w32(REG_GCTL, 0);
    for (uint32_t i = 0; i < 100000; i++) {
        if ((r32(REG_GCTL) & 1) == 0) {
            break;
        }
        io_wait();
    }
    if (r32(REG_GCTL) & 1) {
        return false;
    }
    udelay(200);

    /* 脱离复位（CRST=1） */
    w32(REG_GCTL, 1);
    for (uint32_t i = 0; i < 100000; i++) {
        if (r32(REG_GCTL) & 1) {
            break;
        }
        io_wait();
    }
    if (!(r32(REG_GCTL) & 1)) {
        return false;
    }
    udelay(1000);           /* >521us：等待编解码器完成链路初始化 */
    return true;
}

/* ---- CORB/RIRB 初始化（spec §4.4.1）：各 1 页 DMA 环，256 项 ---- */
static bool hda_ring_init(void)
{
    void *corb = pmm_alloc_page();
    void *rirb = pmm_alloc_page();
    if (!corb || !rirb) {
        return false;
    }
    g_corb_phys = (uint64_t)corb;
    g_rirb_phys = (uint64_t)rirb;
    memset(PHYS_TO_VIRT(g_corb_phys), 0, PAGE_SIZE);
    memset(PHYS_TO_VIRT(g_rirb_phys), 0, PAGE_SIZE);

    /* 停 DMA 后再编程基址 */
    w8(REG_CORBCTL, 0);
    w8(REG_RIRBCTL, 0);
    udelay(50);

    /* CORB：选 256 项（QEMU 支持；容量位见 CORBSIZE[7:4]） */
    uint8_t cap = r8(REG_CORBSIZE);
    if (cap & 0x40) {
        w8(REG_CORBSIZE, 0x2);  g_corb_entries = 256;
    } else if (cap & 0x20) {
        w8(REG_CORBSIZE, 0x1);  g_corb_entries = 16;
    } else {
        w8(REG_CORBSIZE, 0x0);  g_corb_entries = 2;
    }
    w32(REG_CORBLBASE, (uint32_t)g_corb_phys);
    w32(REG_CORBUBASE, (uint32_t)(g_corb_phys >> 32));

    /* CORB 读指针复位：写 bit15，回读确认，再清零（spec 流程；
     * 部分实现不回显 bit15，故轮询有界，超时继续） */
    w16(REG_CORBRP, 0x8000);
    for (uint32_t i = 0; i < 1000; i++) {
        if (r16(REG_CORBRP) & 0x8000) {
            break;
        }
        io_wait();
    }
    w16(REG_CORBRP, 0);
    for (uint32_t i = 0; i < 1000; i++) {
        if ((r16(REG_CORBRP) & 0x8000) == 0) {
            break;
        }
        io_wait();
    }
    w16(REG_CORBWP, 0);

    /* RIRB：256 项，先复位 WP 再复位 RP，使二者均为 0（与软件 g_rirb_rp 对齐）。
     * 注意 0x5A 是 RIRBRP（读指针），不是 osdev 说的 RINTCNT；此处复位 RP。 */
    cap = r8(REG_RIRBSIZE);
    if (cap & 0x40) {
        w8(REG_RIRBSIZE, 0x2);  g_rirb_entries = 256;
    } else if (cap & 0x20) {
        w8(REG_RIRBSIZE, 0x1);  g_rirb_entries = 16;
    } else {
        w8(REG_RIRBSIZE, 0x0);  g_rirb_entries = 2;
    }
    w32(REG_RIRBLBASE, (uint32_t)g_rirb_phys);
    w32(REG_RIRBUBASE, (uint32_t)(g_rirb_phys >> 32));
    w16(REG_RIRBWP, 0x8000);    /* 复位写指针（bit15，自清）：wp = 0 */
    /* 写 RINTCNT（0x5A）= 0xFF。
     * 规范权威定义（《high-definition-audio-specification.pdf》§3.3.28）：
     *   0x5A 即 RINTCNT（Response Interrupt Count）寄存器，绝非 RIRBRP
     *   （规范中本无 RIRBRP 寄存器，RIRB 读指针由硬件隐式维护，软件不写）。
     * QEMU 的 intel-hda 把 0x5A 建模为 rirb_cnt，且 CORB 引擎在
     *   rirb_count == rirb_cnt 时停止；默认 0 会导致 CORB 永不运行，故置 255
     *   使整个枚举/打开阶段（远少于 255 条 verb）都能被控制器消费。
     * 真机同样为 RINTCNT 寄存器（置非零值合理），故 QEMU 与真机在此处一致。 */
    w16(REG_RINTCNT, 0x00FF);
    g_rirb_rp = 0;               /* QEMU 响应落在槽 (wp+1)，读取时加 1 偏移到 */

    /* 启动两个 DMA 引擎 */
    w8(REG_CORBCTL, 0x02);      /* CORBRUN */
    w8(REG_RIRBCTL, 0x02);      /* RIRBDMAEN */
    return true;
}

/* ---- 经 CORB 发送一条 verb 并等待 RIRB 响应 ----
 * cmd = (cad<<28)|(nid<<20)|verb20。失败/超时返回 0xFFFFFFFF。 */
#define HDA_VERB_TIMEOUT  0xFFFFFFFFu

static uint32_t hda_verb_raw(uint32_t cmd)
{
    volatile uint32_t *corb = (volatile uint32_t *)PHYS_TO_VIRT(g_corb_phys);
    volatile uint32_t *rirb = (volatile uint32_t *)PHYS_TO_VIRT(g_rirb_phys);

    /* CORB 用法（对照 QEMU intel_hda_corb_run）：QEMU 把 CORBWP 寄存器当作
     * “最后已写槽”(corb_wp)，内部读指针 corb_rp 复位为 0，循环读取
     * (corb_rp+1)..corb_wp 的 verb。故首条命令必须落在槽 1（而非槽 0）。
     * 做法：把命令写入 (cur+1)%N 槽，并把 CORBWP 置为该槽索引（cur=当前
     * CORBWP）。如此 QEMU 的 (corb_rp=0)+1 .. CORBWP 恰好覆盖刚写入的槽。 */
    uint16_t cur = (uint16_t)(r16(REG_CORBWP) & 0xFF);
    uint16_t slot = (uint16_t)((cur + 1) % g_corb_entries);
    corb[slot] = cmd;
    __asm__ volatile("" ::: "memory");      /* 确保写入先于门铃（CORBWP 写） */
    w16(REG_CORBWP, slot);

    /* 轮询 RIRBWP 前进（100ms 超时）。
     * QEMU 与规范实现一致：RIRBWP 寄存器返回"下一待写"索引（exclusive）。
     * 已写入、待软件消费的条目为 [g_rirb_rp, hw_wp) 开区间。每条 8 字节
     * （response + response_ex 两个 uint32），数组下标用 *2。逐条扫描窗口
     * 内所有条目，跳过 unsolicited（ext 的 S 位 bit4）响应，返回最后一个
     * 非 unsolicited（solicited）响应。单次可能夹带 unsolicited 响应，故
     * 扫描整段窗口而非只读一条。 */
    for (uint32_t i = 0; i < 100000; i++) {
        /* M11 修复：codec 无响应时原 100000 次 io_wait 忙等会长期占死 CPU。
         * 每 8192 次迭代让出一次（单核下切换到其它就绪任务），缩短占死窗口；
         * 控制器响应与 CPU 调度无关，让出不影响 verb 完成。 */
        if ((i & 0x1FFF) == 0) {
            task_yield();
        }
        uint32_t hw_wp = r16(REG_RIRBWP) & 0xFF;
        uint32_t last_resp = 0;
        bool     found = false;
        /* QEMU 把响应写到槽 (rirb_wp+1)（rp/wp 为“最后处理”索引）。
         * 故以 (g_rirb_rp+1)%N 为实际槽号读取，读完令 g_rirb_rp = 该槽号。
         * 有效窗口为 (g_rirb_rp .. hw_wp]，等价于读槽 (g_rirb_rp+1)..hw_wp。 */
        while (g_rirb_rp != hw_wp) {
            uint32_t s   = (g_rirb_rp + 1) % g_rirb_entries;
            uint32_t resp = rirb[s * 2];
            uint32_t ext  = rirb[s * 2 + 1];
            g_rirb_rp = s;
            if (ext & (1u << 4)) {
                continue;                   /* unsolicited：忽略 */
            }
            last_resp = resp;
            found = true;
        }
        if (found) {
            w8(REG_RIRBSTS, 0x5);           /* 清响应状态位 */
            return last_resp;
        }
        io_wait();
    }
    kprintf("[hda] verb %08x TIMEOUT (cad=%u)\n", cmd, g_cad);
    {
        uint32_t hw_wp  = r16(REG_RIRBWP) & 0xFF;
        uint32_t corbrp = r16(REG_CORBRP) & 0xFF;
        uint32_t corbwp = r16(REG_CORBWP) & 0xFF;
        kprintf("[hda]   RIRBWP=%u CORBRP=%u CORBWP=%u g_rirb_rp=%u\n",
                hw_wp, corbrp, corbwp, g_rirb_rp);
        kprintf("[hda]   rirb=[%08x %08x %08x %08x] corb=[%08x %08x %08x %08x]\n",
                rirb[0], rirb[1], rirb[2], rirb[3],
                corb[0], corb[1], corb[2], corb[3]);
    }
    return HDA_VERB_TIMEOUT;
}

/* 12 位 verb（8 位载荷） */
static uint32_t hda_verb(uint32_t nid, uint32_t verb, uint32_t payload)
{
    return hda_verb_raw((g_cad << 28) | (nid << 20) | (verb << 8)
                        | (payload & 0xFF));
}

/* 4 位 verb（16 位载荷）：SET_STREAM_FORMAT / SET_AMP_GAIN_MUTE */
static uint32_t hda_verb4(uint32_t nid, uint32_t verb4, uint32_t payload16)
{
    return hda_verb_raw((g_cad << 28) | (nid << 20) | (verb4 << 16)
                        | (payload16 & 0xFFFF));
}

static uint32_t hda_param(uint32_t nid, uint32_t par)
{
    return hda_verb(nid, VERB_GET_PARAM, par);
}

/* ---- 编解码器拓扑枚举：找 AFG / DAC / 输出 Pin，并接通输出通路 ---- */
static bool hda_codec_setup(void)
{
    /* root(0) 的子节点 = 功能组列表 */
    uint32_t sub = hda_param(0, PAR_SUB_NODE_CNT);
    if (sub == HDA_VERB_TIMEOUT) {
        return false;
    }
    uint32_t fg_start = (sub >> 16) & 0xFF, fg_cnt = sub & 0xFF;

    for (uint32_t fg = fg_start; fg < fg_start + fg_cnt; fg++) {
        uint32_t t = hda_param(fg, PAR_FG_TYPE);
        if ((t & 0x7F) == 0x01) {           /* Audio Function Group */
            g_nid_afg = fg;
            break;
        }
    }
    if (!g_nid_afg) {
        kprintf("[hda] no AFG found on codec %u\n", g_cad);
        return false;
    }
    hda_verb(g_nid_afg, VERB_SET_POWER, 0x00);      /* AFG -> D0 */
    udelay(200);

    /* 遍历 AFG 的 widget：取第一个 DAC 与第一个输出能力 Pin */
    sub = hda_param(g_nid_afg, PAR_SUB_NODE_CNT);
    uint32_t w_start = (sub >> 16) & 0xFF, w_cnt = sub & 0xFF;
    for (uint32_t nid = w_start; nid < w_start + w_cnt; nid++) {
        uint32_t caps = hda_param(nid, PAR_AW_CAPS);
        uint32_t type = (caps >> 20) & 0xF;
        if (type == WTYPE_DAC && !g_nid_dac) {
            g_nid_dac = nid;
        } else if (type == WTYPE_PIN && !g_nid_pin) {
            uint32_t pcap = hda_param(nid, PAR_PIN_CAPS);
            if (pcap & (1u << 4)) {         /* Output Capable */
                g_nid_pin = nid;
            }
        }
    }
    if (!g_nid_dac || !g_nid_pin) {
        kprintf("[hda] codec %u: DAC=%u PIN=%u (incomplete path)\n",
                g_cad, g_nid_dac, g_nid_pin);
        return false;
    }

    /* Pin 的连接列表中定位 DAC，设置连接选择 */
    uint32_t cll = hda_param(g_nid_pin, PAR_CONN_LIST_LEN);
    if (cll == HDA_VERB_TIMEOUT) {           /* L6：响应超时当错误，不臆造 */
        kprintf("[hda] pin %u GET_CONN_LIST_LEN TIMEOUT\n", g_nid_pin);
        return false;
    }
    uint32_t len = cll & 0x7F;
    /* L6 修复：异常 codec 可能报告超大连接列表长度（CONN_LIST_LEN 域仅 7 位
     * 但可被损坏/伪造），导致无界 verb 轮询甚至读取越界的响应窗口。把长度
     * 钳制到 64（真实 codec 连接数极少超过 4），杜绝异常拓扑下的越界读。 */
    if (len > 64) {
        kprintf("[hda] pin %u conn list len %u clamped to 64\n",
                g_nid_pin, len);
        len = 64;
    }
    bool longform = (cll & 0x80) != 0;
    int sel = -1;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t per = longform ? 2 : 4;
        uint32_t ent = hda_verb(g_nid_pin, VERB_GET_CONN_LIST,
                                (i / per) * per);
        if (ent == HDA_VERB_TIMEOUT) {       /* L6：单条连接项超时即放弃 */
            kprintf("[hda] pin %u conn list entry TIMEOUT at %u\n",
                    g_nid_pin, i);
            break;
        }
        uint32_t shift = (i % per) * (longform ? 16 : 8);
        uint32_t mask  = longform ? 0xFFFF : 0xFF;
        uint32_t nid   = (ent >> shift) & mask & 0x7F;
        if (nid == g_nid_dac) {
            sel = (int)i;
            break;
        }
    }
    if (sel >= 0 && len > 1) {
        hda_verb(g_nid_pin, VERB_SET_CONN_SEL, (uint32_t)sel);
    }

    /* 功耗 D0 */
    hda_verb(g_nid_dac, VERB_SET_POWER, 0x00);
    hda_verb(g_nid_pin, VERB_SET_POWER, 0x00);
    udelay(200);

    /* 放大器：0dB 解静音（载荷：bit15 输出|bit13 左|bit12 右|增益）。
     * 0dB 增益 = 输出放大器能力的 Offset 字段（[6:0]）；widget 无自带
     * 能力时回退 AFG 默认值。 */
    uint32_t oc = hda_param(g_nid_dac, PAR_OUT_AMP_CAPS);
    if (oc == 0 || oc == HDA_VERB_TIMEOUT) {
        oc = hda_param(g_nid_afg, PAR_OUT_AMP_CAPS);
    }
    uint32_t gain = oc & 0x7F;
    hda_verb4(g_nid_dac, VERB4_SET_AMP, 0xB000 | gain);
    oc = hda_param(g_nid_pin, PAR_OUT_AMP_CAPS);
    if (oc != 0 && oc != HDA_VERB_TIMEOUT) {
        hda_verb4(g_nid_pin, VERB4_SET_AMP, 0xB000 | (oc & 0x7F));
    }

    /* Pin：输出使能（bit6 OUT_EN；耳机口再加 bit7 HP_EN）+ EAPD */
    uint32_t cfg = hda_verb(g_nid_pin, VERB_GET_CFG_DEF, 0);
    uint32_t dev = (cfg >> 20) & 0xF;
    uint8_t pinctl = 0x40;
    if (dev == 0x2) {                        /* HP Out */
        pinctl |= 0x80;
    }
    hda_verb(g_nid_pin, VERB_SET_PIN_CTL, pinctl);
    hda_verb(g_nid_pin, VERB_SET_EAPD, 0x02);

    kprintf("[hda] codec cad=%u AFG=%u DAC=%u PIN=%u (path connected)\n",
            g_cad, g_nid_afg, g_nid_dac, g_nid_pin);
    return true;
}

/* ---- 输出流寄存器基址 ---- */
static inline uint32_t sd_base(void)
{
    return REG_SD_BASE + g_oss_base_idx * 0x20;
}

/* ---- 采样率 -> 流格式编码（spec §3.7.1）----
 * bit14 BASE(0=48k,1=44.1k) | [13:11] MULT | [10:8] DIV | [6:4] BITS | [3:0] CH-1 */
static uint32_t hda_format(uint32_t rate, uint32_t channels, uint32_t bits)
{
    uint32_t fmt;
    switch (rate) {
    case 48000: fmt = 0x0000;                   break;  /* 48k ×1 /1 */
    case 44100: fmt = 0x4000;                   break;  /* 44.1k */
    case 96000: fmt = 0x0800;                   break;  /* 48k ×2 */
    case 88200: fmt = 0x4800;                   break;  /* 44.1k ×2 */
    case 32000: fmt = 0x0A00;                   break;  /* 48k ×2 /3 */
    case 24000: fmt = 0x0100;                   break;  /* 48k /2 */
    case 22050: fmt = 0x4100;                   break;  /* 44.1k /2 */
    case 16000: fmt = 0x0200;                   break;  /* 48k /3 */
    case 12000: fmt = 0x0300;                   break;  /* 48k /4 */
    case 11025: fmt = 0x4300;                   break;  /* 44.1k /4 */
    case  8000: fmt = 0x0500;                   break;  /* 48k /6 */
    default:    return HDA_VERB_TIMEOUT;
    }
    switch (bits) {
    case 16: fmt |= 0x10; break;                /* [6:4]=001 */
    case 32: fmt |= 0x40; break;
    case  8: break;
    default: return HDA_VERB_TIMEOUT;
    }
    if (channels < 1 || channels > 2) {
        return HDA_VERB_TIMEOUT;
    }
    return fmt | (channels - 1);
}

/* ---- 流复位（SRST 1->0，带界限轮询） ---- */
static void hda_stream_reset(void)
{
    uint32_t b = sd_base();
    w8(b + SD_CTL0, 0);                          /* 先停 RUN */
    udelay(50);
    w8(b + SD_CTL0, 0x01);                       /* SRST=1 */
    for (uint32_t i = 0; i < 10000; i++) {
        if (r8(b + SD_CTL0) & 0x01) {
            break;
        }
        io_wait();
    }
    w8(b + SD_CTL0, 0x00);                       /* SRST=0 */
    for (uint32_t i = 0; i < 10000; i++) {
        if ((r8(b + SD_CTL0) & 0x01) == 0) {
            break;
        }
        io_wait();
    }
}

/* ---- 公共接口 ---- */

bool hda_present(void)
{
    return g_present;
}

bool hda_init(void)
{
    pci_dev_t d;
    if (!pci_find_class(0x04, 0x03, &d)) {
        kprintf("[hda] no HDA controller on PCI bus\n");
        return false;
    }
    uint64_t bar = pci_bar_addr(&d, 0);
    if (!bar) {
        kprintf("[hda] BAR0 invalid\n");
        return false;
    }
    if (bar >= 0x100000000UL) {
        /* 引导页表仅覆盖 0..4GB；QEMU 的 BAR 恒 <4GB */
        kprintf("[hda] BAR0 above 4GB unsupported (%p)\n", (void *)bar);
        return false;
    }
    pci_enable_device(&d);
    g_mmio = (volatile uint8_t *)PHYS_TO_VIRT(bar);

    kprintf("[hda] controller %04x:%04x at PCI %u:%u.%u, MMIO phys=%p\n",
            d.vendor_id, d.device_id, d.bus, d.dev, d.func, (void *)bar);

    if (!hda_controller_reset()) {
        kprintf("[hda] controller reset failed\n");
        return false;
    }

    uint16_t gcap = r16(REG_GCAP);
    uint32_t iss = (gcap >> 8) & 0xF, oss = (gcap >> 12) & 0xF;
    g_oss_base_idx = iss;                        /* 输出流引擎紧跟输入流之后 */
    kprintf("[hda] GCAP=%04x ISS=%u OSS=%u ver=%u.%u\n",
            gcap, iss, oss, r8(REG_VMAJ), r8(REG_VMIN));
    if (oss == 0) {
        kprintf("[hda] no output stream engine\n");
        return false;
    }

    uint16_t statests = r16(REG_STATESTS);
    if (!statests) {
        kprintf("[hda] no codec attached\n");
        return false;
    }
    for (uint32_t i = 0; i < 15; i++) {          /* 取最低位编解码器 */
        if (statests & (1u << i)) {
            g_cad = i;
            break;
        }
    }
    w16(REG_STATESTS, statests);                 /* 写 1 清除 */
    w32(REG_INTCTL, 0);                          /* 轮询模式：全部中断关闭 */
    w16(REG_WAKEEN, 0);

    if (!hda_ring_init()) {
        kprintf("[hda] CORB/RIRB init failed\n");
        return false;
    }
    kprintf("[hda] ring ok: corb_phys=%p rirb_phys=%p cad=%u "
            "corb_entries=%u rirb_entries=%u\n",
            (void *)(uintptr_t)g_corb_phys, (void *)(uintptr_t)g_rirb_phys,
            g_cad, g_corb_entries, g_rirb_entries);

    uint32_t vid = hda_param(0, PAR_VENDOR_ID);
    kprintf("[hda] codec cad=%u vendor/device=%08x\n", g_cad, vid);
    if (vid == HDA_VERB_TIMEOUT) {
        return false;
    }
    if (!hda_codec_setup()) {
        return false;
    }

    /* 预分配 BDL（1 页）与 32 个 4KB 数据页 */
    void *bdl = pmm_alloc_page();
    if (!bdl) {
        return false;
    }
    g_bdl_phys = (uint64_t)bdl;
    memset(PHYS_TO_VIRT(g_bdl_phys), 0, PAGE_SIZE);
    for (uint32_t i = 0; i < HDA_BDL_ENTRIES; i++) {
        void *p = pmm_alloc_page();
        if (!p) {
            /* M12 修复：BDL/数据页分配中途失败需回滚，释放已分配的 BDL 页
             * 与前面各数据页，避免物理页泄漏（此前直接 return 致泄漏）。 */
            for (uint32_t k = 0; k < i; k++) {
                pmm_free_page((void *)g_buf_phys[k]);
            }
            pmm_free_page((void *)g_bdl_phys);
            g_bdl_phys = 0;
            kprintf("[hda] BDL buffer alloc failed, rolled back\n");
            return false;
        }
        g_buf_phys[i] = (uint64_t)p;
        memset(PHYS_TO_VIRT(g_buf_phys[i]), 0, PAGE_SIZE);
    }

    g_present = true;
    kprintf("[hda] ready: output engine #%u, ring %u KiB (polling, no IRQ)\n",
            g_oss_base_idx, (unsigned)(HDA_BUF_BYTES / 1024));
    return true;
}

int hda_pcm_open(uint32_t rate, uint32_t channels, uint32_t bits)
{
    if (!g_present) {
        return -1;
    }
    /* H6 修复：同一时刻仅允许一个任务持有输出流。若已被他人打开，拒绝
     * 本次 open（避免重置 DMA / 覆盖 BDL 破坏正在进行的播放）。owner 自身
     * 重复 open 视为重新配置，放行（将重建 BDL 与流格式）。 */
    task_t *cur = sched_current();
    uint64_t irqf = spin_lock_irqsave(&g_hda_lock);
    if (g_opened && g_owner != cur) {
        uint64_t owner_pid = g_owner ? g_owner->id : 0;
        spin_unlock_irqrestore(&g_hda_lock, irqf);
        kprintf("[hda] pcm open denied: stream owned by pid=%lu\n",
                (unsigned long)owner_pid);
        return -1;
    }

    uint32_t fmt = hda_format(rate, channels, bits);
    if (fmt == HDA_VERB_TIMEOUT) {
        spin_unlock_irqrestore(&g_hda_lock, irqf);
        kprintf("[hda] unsupported format %u Hz %u ch %u bit\n",
                rate, channels, bits);
        return -1;
    }

    hda_stream_reset();

    /* BDL：32 项，每项 {addr64, len32, ioc32}；轮询模式 IOC=0 */
    uint32_t *bdl = (uint32_t *)PHYS_TO_VIRT(g_bdl_phys);
    for (uint32_t i = 0; i < HDA_BDL_ENTRIES; i++) {
        bdl[i * 4 + 0] = (uint32_t)g_buf_phys[i];
        bdl[i * 4 + 1] = (uint32_t)(g_buf_phys[i] >> 32);
        bdl[i * 4 + 2] = PAGE_SIZE;
        bdl[i * 4 + 3] = 0;
        memset(PHYS_TO_VIRT(g_buf_phys[i]), 0, PAGE_SIZE);   /* 静音填充 */
    }

    uint32_t b = sd_base();
    w32(b + SD_BDPL, (uint32_t)g_bdl_phys);
    w32(b + SD_BDPU, (uint32_t)(g_bdl_phys >> 32));
    w32(b + SD_CBL, HDA_BUF_BYTES);
    w16(b + SD_LVI, HDA_BDL_ENTRIES - 1);
    w16(b + SD_FMT, (uint16_t)fmt);
    w8(b + SD_CTL2, 0x10);                       /* 流标签 = 1（[7:4]） */

    /* 编解码器侧：DAC 绑定流 1 通道 0，并设置相同格式 */
    hda_verb4(g_nid_dac, VERB4_SET_FORMAT, fmt);
    hda_verb(g_nid_dac, VERB_SET_STREAM_CH, 0x10);

    g_opened = true;
    g_running = false;
    g_wr_ofs = 0;
    g_owner = cur;                          /* H6：登记当前任务为流 owner */
    spin_unlock_irqrestore(&g_hda_lock, irqf);
    kprintf("[hda] pcm open: %u Hz, %u ch, %u bit (fmt=0x%x) owner pid=%lu\n",
            rate, channels, bits, fmt, (unsigned long)cur->id);
    return 0;
}

/* 已写入但尚未被硬件(LPIB)消费的字节数：直接由"写指针 - 读指针"求差，
 * 绝对自校正，不增量累加，彻底消除旧实现中因 QEMU 的 LPIB 复位/回跳导致
 * g_queued 累计漂移、无符号下溢成极大值、进而覆盖未播放数据的致命缺陷。
 * 未启动 DMA 时读指针视为 0（已写即未播）。 */
/* 预填充水位：累积 48KB 再启动 DMA，避免起播即欠载 */
#define HDA_PREFILL_BYTES  (48 * 1024)
/* 写指针与读指针之间保留的保护间隙（2 页 = 8KB，覆盖 QEMU 音频定时器
 * 的 LPIB 突发前进步长，避免写指针在突发期间追上读指针造成覆盖） */
#define HDA_GUARD_BYTES    (2 * PAGE_SIZE)

static uint64_t hda_queued_bytes(void)
{
    if (!g_running) {
        return g_wr_ofs;                         /* 未运行：已写全部未播 */
    }
    uint32_t lpib = r32(sd_base() + SD_LPIB) % HDA_BUF_BYTES;
    return (g_wr_ofs + HDA_BUF_BYTES - lpib) % HDA_BUF_BYTES;
}

/* 可安全写入的空闲字节（恒在 [0, HDA_BUF_BYTES) 内，绝不会下溢成极大值）：
 * 用当前排队量反推，并保留 GUARD 保护间隙，防止写指针追上读指针。 */
static uint64_t hda_free_space(void)
{
    uint64_t q = hda_queued_bytes();
    if (q + HDA_GUARD_BYTES >= HDA_BUF_BYTES) {
        return 0;
    }
    return HDA_BUF_BYTES - HDA_GUARD_BYTES - q;
}

int64_t hda_pcm_write(const void *buf, size_t len)
{
    uint64_t irqf = spin_lock_irqsave(&g_hda_lock);
    if (!g_present || !g_opened) {
        spin_unlock_irqrestore(&g_hda_lock, irqf);
        return -1;
    }
    /* H6 修复：仅 owner 可写入播放环；非 owner 写入会污染他人流，拒绝。 */
    if (g_owner != sched_current()) {
        spin_unlock_irqrestore(&g_hda_lock, irqf);
        return -1;
    }
    uint64_t free = hda_free_space();
    if (free == 0) {
        spin_unlock_irqrestore(&g_hda_lock, irqf);
        return 0;                                /* 环满：调用方 yield 重试 */
    }
    uint64_t n = len < free ? len : free;        /* n 恒 <= HDA_BUF_BYTES-GUARD */

    const uint8_t *src = (const uint8_t *)buf;
    uint64_t copied = 0;
    while (copied < n) {
        uint32_t page = (uint32_t)(g_wr_ofs / PAGE_SIZE);
        uint32_t off  = (uint32_t)(g_wr_ofs % PAGE_SIZE);
        uint32_t chunk = (uint32_t)(PAGE_SIZE - off);
        if (chunk > n - copied) {
            chunk = (uint32_t)(n - copied);
        }
        memcpy((uint8_t *)PHYS_TO_VIRT(g_buf_phys[page]) + off,
               src + copied, chunk);
        copied += chunk;
        g_wr_ofs = (g_wr_ofs + chunk) % HDA_BUF_BYTES;
    }

    /* 静音前哨：把写指针前方一小段"空闲区"清零。只清空闲区（未被播放、
     * 即将被下次写入覆盖的区域），绝不触碰已排队未播放的数据；万一硬件
     * 在下次写入前追上读指针（欠载），播到的是静音而非陈旧数据。 */
    uint64_t ahead = hda_free_space();
    if (ahead > 8192) {
        ahead = 8192;
    }
    uint64_t z_ofs = g_wr_ofs;
    while (ahead > 0) {
        uint32_t page = (uint32_t)(z_ofs / PAGE_SIZE);
        uint32_t off  = (uint32_t)(z_ofs % PAGE_SIZE);
        uint32_t chunk = (uint32_t)(PAGE_SIZE - off);
        if (chunk > ahead) {
            chunk = (uint32_t)ahead;
        }
        memset((uint8_t *)PHYS_TO_VIRT(g_buf_phys[page]) + off, 0, chunk);
        ahead -= chunk;
        z_ofs = (z_ofs + chunk) % HDA_BUF_BYTES;
    }

    if (!g_running && hda_queued_bytes() >= HDA_PREFILL_BYTES) {
        w8(sd_base() + SD_CTL0, 0x02);           /* RUN=1，启动流 DMA */
        g_running = true;
    }
    spin_unlock_irqrestore(&g_hda_lock, irqf);
    return (int64_t)n;
}

uint64_t hda_pcm_queued(void)
{
    uint64_t irqf = spin_lock_irqsave(&g_hda_lock);
    if (!g_present || !g_opened) {
        spin_unlock_irqrestore(&g_hda_lock, irqf);
        return 0;
    }
    /* H6 修复：仅 owner 可查询/触发其流尾播放；非 owner 返回 0 不误触。 */
    if (g_owner != sched_current()) {
        spin_unlock_irqrestore(&g_hda_lock, irqf);
        return 0;
    }
    /* 尚未启动（数据不足预填充水位）时也要能启动尾声播放 */
    if (!g_running && hda_queued_bytes() > 0) {
        w8(sd_base() + SD_CTL0, 0x02);
        g_running = true;
    }
    uint64_t q = hda_queued_bytes();
    spin_unlock_irqrestore(&g_hda_lock, irqf);
    return q;
}

void hda_pcm_stop(void)
{
    if (!g_present) {
        return;
    }
    uint64_t irqf = spin_lock_irqsave(&g_hda_lock);
    /* H6 修复：仅 owner（或尚未被认领）可停止流；防止任意任务 kill 掉
     * 他人正在进行的播放。非 owner 直接返回，不触动 DMA。 */
    if (g_opened && g_owner != sched_current()) {
        uint64_t owner_pid = g_owner ? g_owner->id : 0;
        spin_unlock_irqrestore(&g_hda_lock, irqf);
        kprintf("[hda] pcm stop denied: stream owned by pid=%lu\n",
                (unsigned long)owner_pid);
        return;
    }
    w8(sd_base() + SD_CTL0, 0);                  /* RUN=0 */
    udelay(100);
    hda_stream_reset();
    g_running = false;
    g_opened = false;
    g_wr_ofs = 0;
    g_owner = NULL;                             /* H6：释放 owner */
    spin_unlock_irqrestore(&g_hda_lock, irqf);
    kprintf("[hda] pcm stopped\n");
}

/* P0-R1：任务退出清理钩子 —— 若退出任务是 PCM 流 owner，停流并释放所有权。
 * 否则 owner 崩溃/退出后 g_owner 悬空指向已释放 task_t，且音频永久锁死。
 * 由 task_exit_current() 调用（与 port_release_owner 同位）。 */
void hda_release_owner(task_t *t)
{
    if (!g_present) {
        return;
    }
    uint64_t irqf = spin_lock_irqsave(&g_hda_lock);
    if (g_owner != t) {
        spin_unlock_irqrestore(&g_hda_lock, irqf);
        return;
    }
    w8(sd_base() + SD_CTL0, 0);                  /* RUN=0 */
    udelay(100);
    hda_stream_reset();
    g_running = false;
    g_opened = false;
    g_wr_ofs = 0;
    g_owner = NULL;
    spin_unlock_irqrestore(&g_hda_lock, irqf);
    kprintf("[hda] stream owner pid=%lu exited, stream released\n",
            (unsigned long)t->id);
}
