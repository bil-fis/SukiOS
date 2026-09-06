# Step 64 — lwIP 2.2.1 移植 + net_server（用户态网络服务）

> 本阶段是「网络子系统」第二步：在 `lib/lwip-2.2.1`（vendor）基础上移植 lwIP，
> 并以用户态服务 `net_server` 运行协议栈，经 **NET_PORT** 调用 Ring0 e1000 驱动收发
> 帧。已端到端验证：lwIP 启动后通过 QEMU user-net 的 DHCP 服务器（10.0.2.2）自动
> 获取 IP **10.0.2.15/24**，证明「e1000 驱动 → NET_PORT → lwIP 协议栈」全链路打通。

---

## 1. 架构选型：NO_SYS=1（raw API）

SukiNative 的 `suki_wait()` 的 `timeout_ms` **尚未实现**（0=无限等待），而 lwIP 的
顺序/socket API（NO_SYS=0）依赖带超时的 mbox 等待来驱动 TCP 定时器。因此选：

- **`NO_SYS=1`**：不使用 lwIP 的 OS 模拟层（无线程/邮箱）。定时器由 net_server
  主循环周期性调用 `sys_check_timeouts()` 驱动；阻塞语义放在 **IPC 层**（客户端等
  net_server 应答），这正契合微内核网络服务的典型结构，且不依赖内核定时器超时。
- 内存走用户态 `malloc/free`：`MEM_LIBC_MALLOC=1` + `MEMP_MEM_MALLOC=1`。
- 仅启用 IPv4 + ARP + ICMP + UDP + TCP + DHCP + DNS；关闭 IPv6/IGMP/AUTOIP/
  SOCKET/NETCONN/STATS/DEBUG（见 `user/lib/lwipopts.h`）。

> 后续若要实现 POSIX `socket()` 语义，将在 NS_PORT 之上叠加：用户态 libc 的 socket
> 调用经 IPC 到 net_server，net_server 用 lwIP raw API 管理 PCB，阻塞 recv 的等待
> 同样落在 IPC 层（客户端阻塞等 net_server 应答）。

---

## 2. 端口分配

| 端口 | 用途 |
|---|---|
| `NET_PORT` (=11) | Ring0 e1000 驱动 `net_srv_task` 服务端（帧收发） |
| `NET_REPLY_PORT` (=12) | net_server 认领，**作为向 NET_PORT 发请求的本地应答端口** |
| `NS_PORT` (=13) | net_server 对外暴露的 socket 服务端口（下一步用） |

三者均在 `kernel/ipc/port.c::ipc_init()` 中预留（`1..NS_PORT`），避免被
`port_allocate()` 动态分配冲突（见 step63 的端口冲突教训）。

---

## 3. 移植层文件清单

| 文件 | 说明 |
|---|---|
| `user/lib/lwipopts.h` | lwIP 配置（见 §1）。`LWIP_ALTCP=0`/`LWIP_STRICMP=0`/`LWIP_STRNICMP=0` 等规避 libc 依赖 |
| `user/lib/arch/cc.h` | `arch/cc.h` 规范实现：PACK_STRUCT 宏、小端、断言、随机数 |
| `user/lib/shims/inttypes.h` | 交叉链（freestanding）缺 `inttypes.h`，提供 PRI* 宏 |
| `user/lib/shims/ctype.h` | 补 `isxdigit`（lwIP `arch.h` 用到），以宏实现免链接依赖 |
| `user/net_server.c` | 核心：lwIP 初始化 + netif 注册 + DHCP + 帧收发循环 |

`cc.h` 必须提供的端口符号（均按要求实现）：
- `sys_now()` — 自启动起的毫秒数（`clock_gettime(CLOCK_MONOTONIC)`），`timeouts.c` 依赖。
- `lwip_port_rand()` — `LWIP_RAND()` 落到此处（xorshift32，种子由 `sys_now` 派生）。
- `sys_prot_t` + `SYS_LIGHTWEIGHT_PROT=0` — 单线程无需真实临界区，保护为 no-op。

---

## 4. net_server 数据流（与 FS_SERVER 同构）

```
lwIP 协议栈 (net_server, Ring3)
   │  low_level_output()  ── 拷贝 pbuf 链成连续帧
   │  net_send_frame()    ── 构造 NET_MSG_SEND 请求
   ▼
mach_msg_send(NET_PORT, msgh_local_port=NET_REPLY_PORT)
   ▼
Ring0 e1000 net-srv_task  ── 实际 DMA 发送
   ── 收包：net_server 轮询 NET_MSG_RECV（内核最多轮询 20ms）──
   ▼
net_recv_frame() 取帧 → pbuf_alloc(PBUF_RAW) → netif->input(ethernet_input)
```

- net_server 是 **NET_PORT 的客户端**：发 GET_MAC/SEND/RECV，以 NET_REPLY_PORT 收应答。
- 内核 `net_srv_task` 才是 NET_PORT 的**服务端**（step63 已落地），只做帧收发、绝不解析协议。
- 主循环每轮：`sys_check_timeouts()` → 轮询 RECV → 收到帧则 `ethernet_input` → `nanosleep(5ms)` 节流并让出 CPU。

---

## 5. 构建集成（Makefile）

- `USER_PROGS += net_server`，自动生成 `user_net_server_start/_end` 并嵌入内核（kmain
  在 `net_srv_start()` 之后 spawn `net-server` 并 `port_grant_send(NET_PORT, net_task)`）。
- `LWIP_SRCS` 列出 NO_SYS=1 下需要的源（core/*.c、core/ipv4/*.c、netif/ethernet.c、dns.c，
  关闭 altcp/ipv6/igmp/autoip）。
- `net_server.elf` 单独链接规则：`$(USER_LIB_OBJS) $(LWIP_OBJS)`，并加
  `-Wl,--allow-multiple-definition`（容忍 lwIP 与用户库个别符号重复，与 FreeType 库处理一致）。
- `net_server.c.o` 与 lwIP 对象用独立编译规则，带 `LWIP_INC`（-I lwip src/include -I user/lib，
  使 `lwipopts.h` 与 `arch/cc.h` 在 include 路径上）。

---

## 6. 踩坑记录

1. **Makefile CRLF 破坏续行**：`cat -A` 发现行尾 `^M`（CR），导致 `USER_CFLAGS` 的续行
   `\` 失效，使其后的 `-include $(CONFIG_H)` 变成**顶层指令** → make 把 `config.h` 当
   makefile 解析 → "missing separator"。用 `tr -d '\r'` 归一为 LF 解决。**根因**：
   首版 Makefile 编辑的 `old_str` 只覆盖到 `USER_CFLAGS` 最后一行，**漏掉了原本续行的
   `-include $(CONFIG_H)`**，使其游离为顶层 orphan。修复：将该 `-include $(CONFIG_H)` 重新
   并入 `USER_CFLAGS`，并删除孤立行。
2. **mach_msg_header 重定义**：`user/lib/suki.h` 与 `include/ipc/port.h` 各有一份同布局
   定义；net_server 同时包含两者 → 冲突。用统一守卫 `MACH_MSG_HEADER_DEFINED` 包裹（谁先包含谁定义）。
3. **freestanding 缺标准头**：补 `inttypes.h`（PRI* 宏）、`ctype.h::isxdigit`（宏）。
4. **`sys_prot_t` 未定义**：NO_SYS 端口需提供该类型；置 `SYS_LIGHTWEIGHT_PROT=0`（no-op 保护）。
5. **`LWIP_NETIF_TX_SINGLE_PBUF=1` 触发 `#error`**：需 `TCP_OVERSIZE`。改为 0（低层
   `low_level_output` 已用 `pbuf_copy_partial` 拼接整帧，无需单 pbuf）。

---

## 7. 验证结果

`qemu ... -device e1000,netdev=net0 -netdev user,id=net0` 无头运行，serial 日志：

```
[boot] net-server spawned (pid=6), lwIP stack coming up
[net] net_server starting (lwIP 2.2.1, NO_SYS raw API)
[net] MAC=52:54:00:12:34:56
[net] DHCP discover sent; awaiting offer...
[net] DHCP BOUND: ip=10.0.2.15 gw=10.0.2.2 mask=255.255.255.0
```

- **零 panic**；系统持续运行（load balance 正常，线程正常退出）。
- DHCP 完整流程（discover→offer→request→ack）证明：lwIP 能发广播帧并经内核 e1000 真实
  DMA 发出，且能把收到应答帧经 NET_PORT 回灌给 lwIP——即**双向数据通路端到端打通**。

---

## 8. 下一步

1. **POSIX 网络 syscall**：在 `NS_PORT` 之上实现 socket/bind/connect/send/recv/...，
   由用户态 libc 经 IPC 调 net_server（raw API 管理 PCB），阻塞 recv 的等待落在 IPC 层。
2. **SukiNative NET 对象**（号位 150+）：复用 NS_PORT 服务，暴露到 SukiNative 对象体系。
3. 端到端应用验证：在 app 里用 socket 连 QEMU 的 10.0.2.2 或外部网络做真实收发包测试。
