# Step 63 — e1000 (Intel 8254x) 网卡驱动 + NET_PORT 内核服务

> 本阶段是「网络子系统」路线的**第一步**（用户明确要求：先实现 e1000 网卡驱动，
> 再补全 POSIX / SukiNative 的网络部分；lwIP 移植见后续 step）。
>
> 成果：**驱动在 QEMU 生产场景中真实工作**——探测到 `8086:100e`(82540EM)、
> 经 EEPROM 读出 MAC `52:54:00:12:34:56`、链路 up、启动自检发出 ARP 请求并
> **收到 64 字节应答（TX/RX 端到端验证通过）**，全程**零 panic**。

---

## 1. 设计定位与分层

严格遵循 SukiOS 混合内核红线（与 ATA/disk-srv 同构）：

```
应用/网络服务                    （Ring3，后续 lwIP 在此）
   │  mach_msg
   ▼
NET_PORT (=3) 内核服务 net-srv   （Ring0 内核任务，原始以太网帧收发）
   │  e1000_send / e1000_recv
   ▼
e1000 硬件驱动                    （Ring0，仅 BAR0 MMIO + 描述符环 DMA）
```

- **Ring0 只做原始帧收发**，绝不解析 IP/TCP/UDP（与「磁盘驱动只做扇区读写，
  FAT32 全在 FS_SERVER」完全同构）；
- 协议栈（lwIP）后续放用户态，经 NET_PORT 收发帧。

---

## 2. 实现依据（本地 osdev 离线副本）

参考 `osdev_wiki/wiki.osdev.org/Intel_8254x`，逐节落地：

| osdev 章节 | 本驱动实现 |
|---|---|
| Detection | `pci_find_class(0x02, 0x00)` 找以太网控制器；校验 vendor==0x8086；**只用 BAR0** |
| Initialization | `pci_enable_device()` → `CTRL.RST` 复位并等自清 → 置 `CTRL.ASDE\|CTRL.SLU` → 经 EERD 读 EEPROM 前 3 word 得 MAC → 写 `RAL0/RAH0`（RAH0 bit31=AV） |
| Ring setup | TX 8 项 / RX 32 项 legacy 描述符，每项 4096B 缓冲；填 T/RDBAL、T/RDBAH、T/RDLEN、T/RDH、T/RDT |
| Packet Transmittion | 填缓冲 → 置 `length`/`cmd=EOP\|IFCS\|RS` → 推进 `TDT` → 等 `DD` |
| Packet Reception | 按 `DD` 判定 → 取数据 → `status=0` 归还 → 置 `RDT`=刚释放项 |
| Interrupt Handling | 采用**轮询**（与 hda.c 一致）：`IMC=0xFFFFFFFF` 屏蔽中断 + 读 ICR 清零 |

寄存器偏移与位定义全部按 osdev 的 "Device Registers" 表与手册 Table 13-2 编写
（`include/kernel/e1000.h` 中逐条注明来源）。

---

## 3. 文件清单

| 文件 | 说明 |
|---|---|
| `include/kernel/e1000.h` | 寄存器偏移/位、legacy TX/RX 描述符结构、环尺寸、驱动 API |
| `include/ipc/net_proto.h` | NET_PORT(=3) 协议：`NET_MSG_GET_MAC/SEND/RECV`，请求/应答结构 |
| `kernel/drivers/e1000.c` | 驱动实现（探测/复位/EEPROM/建环/收发）+ `net_srv_task` 内核服务 |
| `kernel/kmain.c` | `e1000_init()`（HDA 之后、PCI 已就绪）；`net_srv_start()`（late-init，磁盘之后） |
| `Makefile` | 新增 `QEMU_NET := -device e1000,netdev=net0 -netdev user,id=net0`，接入全部 run 目标 |
| `user/lib/suki.h` | 新增 `#define NET_PORT 3` |

---

## 4. 关键实现细节

### 4.1 MMIO 映射
沿用 hda.c 的做法（`PHYS_TO_VIRT(bar)`）：引导页表以 2MB 大页直映 0..4GB，
故 QEMU 下 MMIO 可直接访问；真机应改为 UC 映射（代码注释已注明）。

### 4.2 描述符环
- 环与数据缓冲均用 `pmm_alloc_page()` 分配整页：天然满足「物理连续 + 16 字节对齐」，
  内核侧经 `PHYS_TO_VIRT` 访问，物理地址写入描述符供 DMA 使用；
- 初始化失败时 `rings_free()` **回滚已分配页**，避免物理页泄漏（与 hda.c 的
  BDL 回滚处理同款防御）。

### 4.3 接收路径的防御
- `RCTL` 置 `SECRC`：**硬件剥离 CRC**，使 `length` 即真正帧长，便于协议栈直接取用；
- `e1000_recv()` 对 `len > max` 做截断，**绝不越界写调用方缓冲**。

### 4.4 服务循环的健壮性（重要修复）
`net_srv_task` 中所有「无进展分支」必须 `task_yield()`：

```c
if (ipc_recv_kernel(...) != MACH_MSG_SUCCESS || n < sizeof(mach_msg_header_t)) {
    task_yield();          /* 单核协作式调度：直接 continue 会饿死其余任务 */
    continue;
}
```
单核为协作式调度，若失败分支紧密循环不让出 CPU，会把用户态任务全部饿死，
表现为「系统卡住但定时器中断仍在跳」。

### 4.5 启动自检（真实端到端）
`e1000_self_test()` 构造并发送一个 **ARP 请求**（who-has 10.0.2.2 tell 10.0.2.15），
随后轮询接收（有界 2 秒，全程 `task_yield`）：
- QEMU user 后端自带虚拟网关/DHCP（10.0.2.2），会回应 ARP；
- 收到应答即证明 **TX 与 RX 两条 DMA 通路均真实可用**。

---

## 5. 验证方式与结果

### 构建
```
make iso      # 自动发现 kernel/drivers/e1000.c（Makefile:123 用 find 通配）
```

### 运行（遵循调试铁律：仅 bash + QEMU 自身机制，禁 GDB/外部脚本）
```
qemu-system-x86_64 -machine pc -cpu qemu64 -smp 1 -m 2G -no-shutdown \
  -display none -serial file:/tmp/suki.log -boot d -cdrom build/SukiOS.iso \
  build/disk.img -device e1000,netdev=net0 -netdev user,id=net0
```

### 实测输出（serial 日志）
```
[e1000] controller 8086:100e at PCI 0:3.0, MMIO phys=0x00000000feb80000
[e1000] ready: MAC=52:54:00:12:34:56 link=up (tx=8 rx=32 descriptors, 4096B buffers, polling)
[sched] created task 'net-srv' pid=5 cpu=0 stack=0xffffc0008001a000
[net-srv] serving NET_PORT (kernel-resident, e1000)
[e1000] self-test: tx=ok rx=64 bytes (TX/RX loop verified)
```
- `8086:100e` = Intel 82540EM（QEMU 默认网卡），PCI 0:3.0；
- MAC 与 QEMU 默认值一致，证明 EEPROM(EERD) 读取正确；
- `link=up` 证明 `ASDE|SLU` 生效、PHY 链路建立；
- 自检 `tx=ok rx=64 bytes`：ARP 请求发出、网关应答（42 字节填充至 64）收回。
- **零 panic / 零三重故障**；另用 `qemu -d int -D` 核查中断向量分布，
  除定时器 `v=20` 外无异常向量，确认**不存在未处理中断风暴**。

---

## 6. 排查记录（供后续参考）

### 6.1 编译：`cpu_relax` 未声明
`cpu_relax()` 定义在 `<kernel/spinlock.h>`（非 percpu.h）。补该 include 后链接通过。

### 6.2 无头环境下的 pthread 停顿 —— **确认为既有问题，与本次改动无关**
现象：headless 运行时 POSIX 测试停在 `pthread_create` PASS 之后（`pthread_join`
不返回），日志不再增长。

排查过程：
1. 用 `qemu -d int -D` 取中断向量分布 → 仅定时器 `v=20`，**排除 e1000 中断风暴**；
2. 观察定时器中断时的 RIP 反复落在同一内核地址 → 判定为自旋/阻塞而非崩溃；
3. 给 `net_srv_task` 的失败分支补 `task_yield()` → 现象未变；
4. **二分对照实验**：把 `kmain.c` 中 `e1000_init()` 与 `net_srv_start()` 全部注释掉
   （驱动完全不启用）重新构建运行 → **仍在完全相同位置停住**（242 行 vs 启用时 248 行，
   差值恰为 6 行 e1000 输出）。

结论：该停顿是**本仓库既有的、与网络无关的** pthread/futex 问题（用户以图形界面
手动运行时该测试是通过的，本会话早前亦有一次 headless 跑出 192/0）。**不由 e1000
驱动引入**，不计入本次回归。已恢复全部驱动调用。

---

### 6.3 端口冲突回归：NET_PORT=3 抢占了 DISPLAY_PORT=3（已修复）
现象（用户 `make clean` 后重跑暴露）：
```
display: port claim failed
[syscall] task 'display-server' pid=7 exit(code=1)
[boot] WARN: display-server did NOT become ready within timeout
```
根因：`user/lib/suki.h` 中 **`DISPLAY_PORT` 本就是 3**，而本轮新增的 `NET_PORT` 也定为 3。
内核 `net-srv` 先执行 `port_set_owner(NET_PORT=3, ...)`，随后 `display_server.c:554`
的 `sys_port_claim(DISPLAY_PORT)` 因端口已被占用而失败，显示服务退出。

修复（4 处）：
1. `include/ipc/port.h`：`NET_PORT` 定为 **11**（1..10 已被 DISK..FONT 占满），并作为
   **规范定义**（与知名端口表同源）；
2. `include/ipc/net_proto.h`：删除自有的 `NET_PORT` 定义，改为 `#include <ipc/port.h>`，
   避免同一常量在两处漂移；
3. `user/lib/suki.h`：`NET_PORT 3 -> 11`，并注明"不得占用 3（DISPLAY_PORT）"；
4. `kernel/ipc/port.c`：`ipc_init()` 的预留上界由 `FONT_PORT` 提升为 **`NET_PORT`**——
   否则 11 仍是空闲槽，会被 `port_allocate()`（`PORT_FIRST_DYN=9`）当动态端口分配出去，
   造成新的冲突。

验证：
```
[ipc] port table ready (64 slots, well-known 1..11, 0=sentinel)
display: fb mapped 1280x720@32 at user va
[display] active: kernel console text now routed to display server
[boot] display-server ready (g_display_active=1, waited=0 yield rounds)
[e1000] self-test: tx=ok rx=64 bytes (TX/RX loop verified)
=== POSIX test summary: PASS=192 FAIL=0 ===
```
显示服务恢复正常，e1000 不受影响，完整回归 PASS=192 FAIL=0、零 panic。

## 7. 后续（下一步）

1. 移植 lwIP（`lib/lwip-2.2.1` 已 vendor）：建 `lwipopts.h` + `sys_arch`（用 SukiOS
   既有 task/sem 原语），并加入 Makefile 构建；
2. Ring3 网络服务：经 NET_PORT 收发原始帧，向上提供 socket 能力；
3. POSIX 网络 syscall（socket/bind/connect/send/recv/...）；
4. SukiNative 网络对象（号位 150+）。
