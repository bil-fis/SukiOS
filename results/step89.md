# step89 — 修复 socket recv/connect 永久挂起（消除 `exec bin/curl` 卡死无返回）

## 一、现象

继 step88 修复 `exec` 路径探测后，`exec bin/curl https://bilibili.com` 能正常装载 `curl.SKA` 并 spawn，但之后**没有任何返回信息、shell 也不回到提示符**——任务永久卡住。

## 二、根因

`kernel/net/socket.c` 的 `net_rpc()` 用 `ipc_recv_kernel(rp, ..., true)` **无条件阻塞**等待 net_server 的应答。对挂起式操作（`RECVFROM`/`RECV`/`CONNECT`/`ACCEPT`），net_server 是把请求挂起、等数据/回调到达才回送应答的；若对端始终不回数据（连接实际未通、TLS 未完成、或网络无回包），这个等待**永远不返回**。

而 `user/lib/suki_native.c:82` 的 `SukiWait()` 明确 `(void)timeout_ms; /* 暂未支持超时 */`；`lwipopts.h` 注释也指出 `SukiWait() 的 timeout_ms 尚未实现`。因此阻塞式 socket 读没有任何超时保护，curl 一旦进入等待即冻结整个调用任务，连带 `exec` 的 `sys_wait` 也阻塞，shell 永远回不来——表现为“无任何返回”。

## 三、澄清：网络栈本身是健康的

headless 启动回归（串口日志）证明数据通路没问题：

```
[nettest] recvfrom() got 21 bytes from 10.0.2.2:69
[nettest] TFTP ERROR reply received (recv path OK)
[nettest] POSIX UDP round-trip (with reply): PASS
[nettest] libc-net API: PASS=4 FAIL=0  ALL OK
[net] DHCP BOUND: ip=10.0.2.15 gw=10.0.2.2 mask=255.255.255.0
```

`recv` 回程、DHCP、libc 网络 API 全部正常。自测仅打 `10.0.2.2:8000/8443`（无服务器，故预期连接失败），**未验证真实外网出口**（bilibili.com 需经 QEMU user-net 的 DNS 代理 10.0.2.3 + NAT 出网，取决于宿主机的实际网络环境）。所以用户之前看到的“卡死”是**等待无上限**所致，而非数据通路损坏。

## 四、修复（`kernel/net/socket.c`）

将 `net_rpc` 的无界阻塞改为**有界等待**：保留每次请求分配回程端口并向 `NS_PORT` 发送，但接收应答改为非阻塞轮询 + `task_yield()`，超时（默认 `NET_RPC_TIMEOUT_NS = 30s`）后返回 `-SUKI_ETIMEDOUT`，由上层 syscall 翻译成 `EAGAIN/ETIMEDOUT` 上抛。这样：

- 正常有数据的 `recv` 立即返回，行为不变；
- 等不到数据的 `recv`/`connect` 在 30s 后优雅失败，调用任务与 shell 不再冻结；
- 仅 socket 网络 RPC 受影响，其它 IPC（FS/NET 帧/控制台）路径不动。

关键改动：

```c
#include <kernel/clock.h>   /* clock_monotonic_ns */

#ifndef NET_RPC_TIMEOUT_NS
#define NET_RPC_TIMEOUT_NS (30ULL * 1000000000ULL)   /* 30 秒 */
#endif

static int net_rpc(uint32_t op, const void *payload, uint32_t req_size)
{
    ... port_allocate / 发送 ...
    uint64_t deadline = clock_monotonic_ns() + NET_RPC_TIMEOUT_NS;
    int rc = -SUKI_ETIMEDOUT;
    for (;;) {
        uint32_t got = 0;
        if (ipc_recv_kernel(rp, net_resp_buf(), (uint32_t)NET_RPC_MAX, &got, false)
                == MACH_MSG_SUCCESS) { rc = 0; break; }
        if ((int64_t)(deadline - clock_monotonic_ns()) <= 0) break;
        task_yield();
    }
    port_free(rp);
    if (rc != 0) return rc;
    ... 解析应答 ...
}
```

`clock_monotonic_ns()` 与 `task_yield()` 均为内核既有可用符号（同 `sys_poll`、`futex` 等处已用），无新增依赖。

## 五、验证

- `make disk` / `make iso`：**0 warning / 0 error**（`socket.c.o` 增量重编通过）。
- headless 启动回归（`-serial file:` + SIGINT 优雅退出）：**无 panic / 无三重故障 / 无 #GP/#PF**；`SukiOS:/>` 提示符正常到达；`nettest` UDP 收发往返 `PASS=4 FAIL=0`、DHCP 正常——系统稳定，未引入回归。
- 子模块保持原始版本（`git submodule status` 无 `+`）。

## 六、用户侧下一步

重新在图形 QEMU 中执行 `exec bin/curl https://bilibili.com`：

- 现在**不会再冻结**——若等不到数据，约 30s 后 curl 会返回明确错误（如 `Could not resolve host` / `Connection timed out` / `SSL` 相关）并回到 shell；
- 若成功取回页面，HTML 会经 TTY 管道由 shell 渲染到终端；
- 若返回错误，错误文本即指向下一层问题（DNS=10.0.2.3 代理是否可达 / TCP NAT 出网是否通 / mbedTLS 熵与握手），据此再定点修复即可。真实外网出口是否可用取决于宿主机网络与 QEMU user-net 的 DNS/NAT 行为，需由交互式运行确认。
