# step65 — 网络子系统 flakiness 根因定位与修复

## 一、现象

网络子系统（net_server + nettest）启用后，开机自检 `posixtest` 出现**系统级随机失败**：
FAIL 计数在 16–36 之间抖动，且禁用 net_server 后立刻稳定在 FAIL=3（仅进程/信号类），
启用即恶化。QEMU `-d int` 日志**无任何 #GP/#PF**，说明不是崩溃，而是逻辑性错误。

临时在 `fs_rpc` 加诊断打印后，发现 FS_SERVER 对合法操作返回了一致的逻辑错误
（如 `stat(/README.TXT)` 返回 `FS_ERR_NOENT`、`close` 返回 `FS_ERR_BADF`），
且失败**横跨所有子系统**：`getpid`、`waitpid`、`sigaction/kill`、`write/fstat`、
`readdir/mkdir/rmdir`、`suki_file_*`、`suki_proc_create` 等。

结论：这不是 FS 特有问题，而是**内核共享结构（任务结构体 / 系统调用分发表）被逻辑性破坏**。

## 二、根因定位（对照上次稳定提交 b604e1d）

按项目铁律「先 `git diff` 对比上一稳定提交」，核心手段是 diff 本期改动：

### 缺陷 1：系统调用号碰撞（17/18）— 导致系统级 flakiness 的主因

`include/sukios/posix.h` 中新增：
```c
#define SYS_PORT_ALLOC  17   /* 与 SYS_FORK 同号 */
#define SYS_PORT_FREE   18   /* 与 SYS_GETPID 同号 */
```
而 `b604e1d` 已将 17/18 分配给核心 ABI：
```c
#define SYS_FORK   17
#define SYS_GETPID 18
```
两个不同名字的宏映射到**同一号位**（C 预处理同名宏值相同不报错，但语义冲突）。

`kernel/syscall/syscall.c` 的 `syscall_dispatch` 结构是：
```c
switch (num) {
    ...
    case SYS_PORT_ALLOC:  return sys_port_alloc();   // 拦截了 17
    case SYS_PORT_FREE:   return sys_port_free(a1);  // 拦截了 18
    ...
    default:              return posix_dispatch(num, ...);
}
```
`case SYS_PORT_ALLOC` 的 17 **抢先于** `default→posix_dispatch(17)` 命中，于是：
- 任意任务调用 `fork()`（号 17）→ 实际执行 `sys_port_alloc()` → 返回端口号而非 PID；
- 任意任务调用 `getpid()`（号 18）→ 实际执行 `sys_port_free()` → 恒返回 0。

结果 `getpid > 0` 失败、`fork` 返回垃圾值、`waitpid/signal/suki_proc_create`
（均依赖 fork/getpid）连锁失败，形成观察到的「全子系统随机失败」。该碰撞是**编译期常量问题**，
与 net_server 是否运行无关——禁用 net_server 时仅暴露那 3 个进程/信号类失败（被误判为「稳定」），
启用 net_server 后更多依赖 fork 的测试（含 FS 测试内的子进程隔离）一并失败，表现为 flakiness。

### 缺陷 2：UDP socket 创建时未注册接收回调 — 导致 nettest recvfrom 永久阻塞

`user/net_server.c` 的 `SOCK_MSG_CREATE` 对 DGRAM 只调用了 `udp_new()`：
```c
if (ns->type == SUKI_SOCK_DGRAM) ns->pcb.udp = udp_new();
```
**从未调用 `udp_recv(pcb, udp_recv_cb, ns)`**。对比 TCP 路径（`tcp_connect`/`complete_accept`
里都调了 `tcp_recv`）——UDP 的接收回调被遗漏。

后果：UDP 帧到达时 lwIP 在该 pcb 上找不到 recv 回调，直接丢弃报文，
`udp_recv_cb` 永不触发 → `try_complete_recv`/`complete_recv_now` 永不执行 →
nettest 的 `recvfrom()` 在 `sys_net_dispatch` 内阻塞等待 `SOCK_MSG_REPLY`，永久挂死。
（DHCP 之所以正常，是因为其 pcb 由 lwIP 内部在 `dhcp_start` 时注册了 `dhcp_recv`，
与用户 socket 的 pcb 无关。）

## 三、修复

**缺陷 1**（`include/sukios/posix.h`）：将 `SYS_PORT_ALLOC/FREE` 改到 88–129 区段的空闲号位
97/98，并加注释警示 17/18 已被核心 ABI 占用、绝不可复用：
```c
#define SYS_PORT_ALLOC  97   /* 动态端口分配，返回端口号（0=失败） */
#define SYS_PORT_FREE   98   /* 释放动态端口（a1=端口号） */
```
`syscall.c` 的 `case SYS_PORT_ALLOC`/`case SYS_PORT_FREE` 与 `suki_native.c` 的调用
均用宏名引用，号位变动自动一致。

**缺陷 2**（`user/net_server.c` 的 `SOCK_MSG_CREATE`）：创建 UDP pcb 后立即注册回调：
```c
if (ns->type == SUKI_SOCK_DGRAM) {
    ns->pcb.udp = udp_new();
    if (ns->pcb.udp) udp_recv(ns->pcb.udp, udp_recv_cb, ns);
} else {
    ns->pcb.tcp = tcp_new();
}
```

（附带：保留 `sched.c` 的 `f_entry` 修复——`task_exit_current` 原先在持 `g_sched_lock`
状态下调用 `fd_exit_task`（经 IPC 阻塞 → `schedule`），会把锁「带走」到被切走的任务造成死锁；
本修复在 `fd_exit_task` 前后释放/重取锁，`f_entry` 保持原始中断语义不变。这是 nettest 退出时
关闭 socket fd 能正常完成的必要修复。）

## 四、验证（bash + QEMU，无外部调试器）

全量网络场景（net_server + nettest 均启用，`-device e1000 -netdev user,id=net0,tftp=/tmp`）：

```
[net] DHCP BOUND: ip=10.0.2.15 gw=10.0.2.2 mask=255.255.255.0
[nettest] socket() ok fd=3
[nettest] sendto(TFTP RRQ) nwritten=20
[nettest] recvfrom() got 83 bytes from 10.0.2.2:69
[nettest] TFTP reply opcode=3
[nettest] POSIX UDP round-trip (with reply): PASS
[nettest] suki_socket_create ok handle=2
[nettest] suki_socket_close st=0
[nettest] all done.
[syscall] task 'nettest' pid=12 exit(code=0)
=== POSIX test summary: PASS=192 FAIL=0 ===
```

- `posixtest` 全绿（192/192，含此前失败的 getpid/waitpid/sigaction/readdir/suki_proc_create 等）；
- nettest UDP 收发往返完整成功（收到 slirp TFTP 的 83 字节 DATA 应答）；
- nettest 干净退出（code=0，socket fd 经 `fd_exit_task` 关闭无死锁）；
- 无 #GP/#PF，生产场景零 panic。

## 五、对照参考

- 采用「先 diff 上一稳定提交定位」的既定排障铁律（非凭记忆硬编码）。
- 系统调用号分配遵循 OSDev/x86 多路复用惯例：核心 ABI（fork/getpid 等）固定低位，
  扩展调用置于明确空闲区段，避免与既有号位冲突（类似 Linux `asm/unistd` 的分区管理）。
- lwIP `udp_new()` 后须显式 `udp_recv()` 注册回调（raw API 语义），见 lwIP raw UDP 文档：
  仅 `udp_new()`+`udp_bind()` 不足以接收，必须有 `udp_recv(pcb, recv_fn, arg)`。

## 六、对照 osdev / Linux 源码 / 网络检索的权威佐证

本轮修复后，按用户要求对照权威资料核验根因分析与号位选择的正确性（非仅凭记忆）。

### 6.1 osdev_wiki（本地离线副本 `wiki.osdev.org/System_Calls`）
- 确认 x86-64 系统调用**标准约定**：调用号经 `%rax` 传递，参数依次经 `%rdi/%rsi/%rdx/%r10/%r8/%r9`
  （System V AMD64 约定）；SukiOS 的 `syscall` 入口（`%rax=num`, `%rdi..%r9` 参数）完全符合。
- 明确「若所有函数号为小而连续的整数，更适合用函数表/分发」——印证 `syscall_dispatch` 的
  `switch(num)` + `default` 结构是标准做法；而正因如此，**号位必须唯一**，否则 `case` 会被错误提前命中
  （这正是缺陷 1 的机理：17 的 `case SYS_PORT_ALLOC` 抢在 `default→posix_dispatch(17)` 之前）。

### 6.2 网络检索（lwIP raw UDP 权威行为）
- mbed OS lwIP 文档：`udp_recv(pcb, recv, recv_arg)` 的作用是「为 UDP PCB 设置接收回调，
  **收到报文时该回调才会被调用**」。TI SPNA248 应用笔记的 raw UDP 流程亦为
  `udp_new → udp_bind → … → udp_recv` 必经步骤。
- 直接印证缺陷 2 的根因：仅 `udp_new()` 而未 `udp_recv()` 注册的 pcb，lwIP 收到 UDP 帧时无回调可触发，
  报文被静默丢弃——与实测 nettest `recvfrom` 永久阻塞完全吻合。补上 `udp_recv(ns->pcb.udp, udp_recv_cb, ns)`
  即符合该标准做法。

### 6.3 查看 Linux 源码（x86_64 syscall 号位表，经 filippo.io 6.16-rc1 与 cnblogs 4.7 镜像）
- **核心进程调用集中在低位专属区**：`fork=57`、`getpid=39`、`clone=56`、`execve=59`、`kill=62`，
  网络 socket 系列连续占据 `41–55`，且存在保留空洞（如 `uselib(134)`、`create_module(174)` 等）。
- 印证「核心 ABI 号位专属化、扩展子系统隔离号段」是成熟内核的设计原则——与把 `PORT_ALLOC/FREE`
  从与 `fork/getpid` 冲突的 17/18 移走的修复方向一致。
- **关于 97/98**：Linux 在该处是 `getrlimit(97)`/`getrusage(98)`，但 SukiOS 为独立 ABI
  （其 `getrlimit=37`、`getrusage=39`），经全量号位扫描 SukiOS 内部 97/98 唯一空闲；且 `SYS_PORT_ALLOC/FREE`
  是**内核 IPC/mach-port 动态端口分配**（非网络 socket），故不归入 150–166 网络区。因此复用 97/98 在
  SukiOS 内无冲突；仅当未来承诺 Linux 二进制兼容时才需另作映射（已在 `posix.h` 注释中载明）。

### 6.4 全量号位冲突复扫（防止同类遗漏）
对照上次稳定提交后新增的全部 `SYS_*` 宏（含 `SYS_MOUSE_READ=204`、`SYS_CLONE=205`、
`SYS_SIGACTION=206`、`SYS_FRAMEBUFFER_MAP=200`、`SYS_SUKI_*=130–149`、`SYS_SOCKET..=150–166` 等），
按号位排序**：当前无任何号位重复**，确认除已修复的 17/18 外无其他碰撞。
