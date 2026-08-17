# Step 19：P0-7 AHCI SATA 驱动——中断驱动 DMA 取代 ATA PIO

> 对应 `results/step10_todos.md` P0-7 里程碑。上一批（step18）完成 P0-5
> VMA/按需分页/COW/mmap。本批实现 AHCI 1.3 控制器驱动：PCI 枚举、端口
> 初始化、命令列表/FIS/PRDT DMA 结构、IOAPIC 中断路由、读/写/FLUSH 命令，
> 并与既有 ATA PIO 组成"AHCI 优先、PIO 回退"的块设备统一分发。

---

## 1. 总览

| 子项 | 状态 | 说明 |
|---|---|---|
| PCI AHCI 控制器探测 | ✅ | class 0x01 / subclass 0x06，ABAR=BAR5 MMIO |
| 端口初始化 | ✅ | 规范顺序：停 ST/FRE → 设 CLB/FB → 清 SERR/IS → 开 FRE/ST |
| DMA 命令执行 | ✅ | 命令列表/命令表/PRDT，slot 0 单命令，4KB 弹跳页 |
| 中断驱动完成通知 | ✅ | PxIE+GHC.IE，PCI INTx→IOAPIC（GSI 10→向量 42），实测 8 次 IRQ |
| IDENTIFY 容量 | ✅ | LBA48 word100-103 优先，回退 word60-61 |
| 读/写/FLUSH | ✅ | READ/WRITE DMA EXT (0x25/0x35) + FLUSH CACHE EXT (0xEA) |
| 块设备统一分发 | ✅ | disk-srv 经 blk_read/blk_write 自动选路，IPC 协议零改动 |
| 验证 | ✅ | LBA0 55AA 签名 / 末扇区写-读回-恢复 PASS / FAT32 挂载 / IDE 回归 |

文件清单：
- 新增 `include/kernel/ahci.h` — 驱动接口
- 新增 `kernel/drivers/ahci.c` — AHCI 驱动实现
- 修改 `kernel/drivers/ata.c` — `blk_read`/`blk_write` 统一分发
- 修改 `kernel/kmain.c` — `ahci_init()` 优先探测
- 修改 `Makefile` — `QEMU_AHCI_DISK` 变量、`run-ahci`/`run-ahci-headless` 目标

---

## 2. 数据结构与关键常量

### 2.1 物理 DMA 结构布局（两页，pmm_alloc_page 保证页对齐+清零）

```
结构页 A（g_structs_phys）：
  +0x000  命令列表 Command List（32 头×32B=1KB，要求 1KB 对齐 ✓）
  +0x400  接收 FIS 区 Received FIS（256B，要求 256B 对齐 ✓）
  +0x800  命令表 Command Table（CFIS 64B + ACMD 16B + 保留 48B + PRDT@0x80）
  +0xC00  自检暂存（LBA0 读缓冲 / 写测试原值备份）
弹跳页 B（g_bounce_phys）：4KB DMA 数据缓冲
```

- 弹跳页设计：`DISK_MAX_SECTORS=7`（IPC 协议单次上限）→ 3.5KB < 4KB 单页，
  避免内核堆"虚拟连续但物理不连续"的 PRDT 多表项复杂度；单 PRDT 表项。
- `AHCI_MAX_SECT=8`：弹跳页容量上限。

### 2.2 寄存器（ABAR 偏移）

| 寄存器 | 偏移 | 用途 |
|---|---|---|
| CAP/GHC/IS/PI | 0x00/0x04/0x08/0x0C | 全局控制：GHC.AE(bit31) AHCI 使能、GHC.IE(bit1) 中断使能 |
| PxCLB/PxFB | port+0x00/0x08 | 命令列表/接收 FIS 物理基址 |
| PxIS/PxIE | port+0x10/0x14 | 端口中断状态(W1C)/使能：DHRS|PSS|DPS|TFES |
| PxCMD | port+0x18 | ST(bit0)/FRE(bit4)/FR(bit14)/CR(bit15) |
| PxTFD/PxSIG/PxSSTS | port+0x20/0x24/0x28 | 任务文件/签名(SATA 盘=0x101)/链路(DET=3 在位) |
| PxSERR/PxCI | port+0x30/0x38 | SATA 错误(W1C)/命令发射位图 |

端口基址 = ABAR + 0x100 + port×0x80，经 `PHYS_TO_VIRT` 内核直映访问
（volatile 指针保序）。

### 2.3 H2D 命令 FIS（20B，CFL=5 DW）

`[0]=0x27 [1]=0x80(C=1) [2]=cmd [4..6]=LBA[0:23] [7]=0x40(LBA模式)
[8..10]=LBA[24:47] [12..13]=count` — 全命令走 LBA48 EXT 系（0x25/0x35/0xEA）。

---

## 3. 算法与关键路径

### 3.1 初始化（ahci_init）

1. `pci_find_class(0x01,0x06)` → `pci_enable_device`（MEM+Bus Master=DMA）→
   ABAR=BAR5。
2. GHC.AE 置位；扫 PI 位图，选第一个 `SSTS.DET==3 && SIG==0x00000101` 端口。
3. 规范停启序列：清 ST 等 CR 清 → 清 FRE 等 FR 清 → 写 CLB/FB → W1C
   SERR/IS → 开 FRE → 开 ST。
4. 中断路由：GSI=PCI INT_LINE（SeaBIOS 已编程，QEMU i440FX/PIIX3 实测
   GSI 10）→ `ioapic_route(gsi, 32+gsi, level, active_low)` + 
   `register_interrupt_handler(42)`；PxIE=DHRS|PSS|DPS|TFES；GHC.IE=1。
5. IDENTIFY(0xEC) 取容量 → LBA0 签名自检 → 末扇区写测试。

### 3.2 命令执行（ahci_exec）——中断 + 轮询双保险

```
构造命令头(slot0, CFL=5, W 位, PRDTL=bytes?1:0)
构造命令表(H2D FIS + PRDT[DBA=弹跳页, DBC=bytes-1, I=1])
等 PxTFD.BSY|DRQ 清零 → g_cmd_done=false → 内存栅栏 → PxCI=1
等待循环：
  g_cmd_done（IRQ 置位）或 PxCI bit0 清零（轮询兜底）→ 完成
  PxIS.TFES → 错误
  TSC 超时（~2s）→ 失败
尾部：幂等 W1C PxIS/IS；TFES 或 TFD.ERR → 报错返回 false
```

设计要点：**中断丢失只降速不失败**（轮询兜底路径完全等价），这对
实机上 PIRQ 路由差异（GSI 极性/触发方式因板而异）是关键容错。

### 3.3 FLUSH 语义

`ahci_write_sectors` = memcpy 到弹跳页 → WRITE DMA EXT → FLUSH CACHE EXT
（PRDTL=0 无数据命令），与 ata.c 的"写后 0xE7 落盘"语义一致（掉电安全）。

### 3.4 统一分发（ata.c::blk_read/blk_write）

`ahci_present() ? ahci_* : ata_*`——disk-srv 消息循环与 DISK_PORT IPC 协议
（`disk_read_req_t` 等）零改动，FS_SERVER 完全无感知。

---

## 4. 验证方式与结果

### 4.1 AHCI 路径（`make run-ahci-headless`）

```
[ahci] IRQ routed: GSI 10 -> vector 42 (level, active-low)
[ahci] port 0 online: 131072 sectors (64 MiB), LBA0 sig OK(55AA), wr-test PASS, irqs=8
[ata] no device on primary master        ← PIO 正确让位
[fs] FAT32 mounted: spc=1 fat@32 data@2050 root_clus=2   ← 全盘 I/O 走 DMA
```

- **irqs=8**：8 条命令（IDENTIFY+LBA0 读+写测试 4 命令含 FLUSH×2）每条
  实触发中断——证明 IOAPIC 路由/向量/EOI 全通，非轮询兜底。
- **wr-test PASS**：末扇区读原值→写图案→读回比对→恢复原值全成功。
- FAT32 挂载 + 目录列举 + shell 启动 = 真实负载读路径验证。

### 4.2 IDE PIO 回归（`make run-headless`，默认）

```
[ahci] no AHCI controller on PCI bus     ← 干净回退
[ata] primary master OK: 131072 sectors (64 MiB) lba48=1
[fs] FAT32 mounted ...                    ← PIO 路径零回归
```

### 4.3 QEMU 调试命令

```bash
make run-ahci                 # 图形界面 AHCI 路径
make run-ahci-headless        # 串口日志验证
# GDB 观察命令发射：
qemu-system-x86_64 -s -S -machine pc -cpu qemu64 -m 2G \
  -cdrom build/SukiOS.iso -device ahci,id=myahci \
  -drive file=build/disk.img,format=raw,if=none,id=ahdisk0 \
  -device ide-hd,drive=ahdisk0,bus=myahci.0
gdb build/kernel.elf -ex 'target remote :1234' -ex 'b ahci_exec' -ex c
```

## 5. 设计边界（诚实声明）

- **单端口单命令槽**：NCQ（多槽并发）未实现——当前 IPC 磁盘协议本身
  串行（disk-srv 单任务），多槽无收益；实机多盘场景 P2 再扩展。
- **弹跳页拷贝开销**：每次 I/O 多一次 3.5KB memcpy；换取 PRDT 单表项的
  简洁与物理连续性保证。零拷贝（用户页直接 DMA）依赖 IOMMU/固定页，远期。
- **MSI 未启用**：走传统 INTx→IOAPIC；MSI 需 PCI capability 遍历，P1 补。
- **QEMU `-device ahci` 即 ICH9 AHCI 挂 i440FX**：实机上控制器可能要求
  BIOS 已切 AHCI 模式（非 IDE 兼容模式），初始化序列已按规范实现。
