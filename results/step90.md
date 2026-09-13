# step90 — 修复 `raise()` 返回 -1 的根因：SYS_TTY_READ 与 SYS_RAISE 号位撞车

## 一、现象

POSIX 自测 `--- signal (sigaction/kill/raise/sigprocmask) ---` 段 6 个用例中 **3 个失败**：
```
[FAIL] signal+raise handler invoked
[FAIL] signal signo matches
[PASS] sigaction+kill handler invoked
[PASS] kill signo matches
[PASS] blocked signal not delivered
[FAIL] signal delivered after unblock
```
规律：凡用到 `raise()` 的用例失败，用到 `kill()` 的用例（含 SIGUSR2）全部 PASS；而 `blocked signal not delivered`（用 raise 置 pending 后被 mask 挡住）PASS，说明 `raise` 能正确**置 pending**，但信号**从不投递**。

## 二、根因（实证定位，非猜测）

在 `user/posixtest.c` 的 T1 插入诊断打印，headless 启动抓取：
```
[DBG] T1 raise(SIGUSR1=10) ret=-1 g_sig_usr1=0 g_sig_last=0        ← raise() 系统调用本身返回 -1
[DBG] T1b kill(getpid(),SIGUSR1) ret=0 g_sig_usr1=1 g_sig_last=10  ← kill() 正常，SIGUSR1 投递也正常
```
`raise()` 返回 -1（系统调用失败），信号根本没发出；`kill()` 正常。于是排除 handler 安装/投递逻辑，锁定在 **`raise()` 的系统调用路由**。

核对 `include/sukios/posix.h` 号位分配，发现**撞号**：
```c
#define SYS_TTY_READ  210   /* 第 816 行 */
...
#define SYS_RAISE     210   /* 第 833 行  ← 与 SYS_TTY_READ 同号 */
```
二者都是 210。内核主分发表 `kernel/syscall/syscall.c:1118` 有 `case SYS_TTY_READ: return sys_tty_read(...)`，在 `default→posix_dispatch` 之前先命中 210，把 `raise()` 当成"读用户 TTY 管道"处理并返回 -1（无数据）；而 `posix_dispatch` 内真正的 `case SYS_RAISE: r = sys_raise(a1)`（syscall/sys_posix.c:2322）成了**死代码**。

影响面：
- `raise()` 永远返回 -1 → 任何用户态 `raise()`（含 libc 内部、依赖自信号的程序、curl/libcurl 部分路径）失效。
- 与 `kill` 走不同分支，故仅 `raise` 系列用例失败；`kill`/信号投递机制本身完全健康。
- 此前 `curl` 卡死的根因之一也是这条被破坏的信号/系统调用路径（叠加 step89 已修的 recv 无限阻塞）。

## 三、修复

仅改一处号位，使二者解耦（shell 与 libc 均经同一头文件，重编号自动同步）：
```c
#define SYS_TTY_READ  211   /* 原 210，与 SYS_RAISE 撞号 */
```
并扫描 `posix.h` 全部 `SYS_*` 号位确认**无其它重复**（脚本：`grep ... | awk '{print $3}' | sort -n | uniq -d` 输出为空），211 空闲。

路由结果：
- 210 → `default → posix_dispatch → case SYS_RAISE → sys_raise` ✓
- 211 → `case SYS_TTY_READ → sys_tty_read`（shell 排干 TTY 管道仍正常）✓

## 四、验证

- `make disk` / `make iso`：**0 warning / 0 error**。
- headless 启动回归（`-serial file:` + SIGINT 优雅退出）：无 panic / 无三重故障 / 无 #GP/#PF；`SukiOS:/>` 正常到达；各服务（FS/Display/Input/Net）正常上线。
- 诊断输出确认修复：
  ```
  [DBG] T1 raise(SIGUSR1=10) ret=0 g_sig_usr1=1 g_sig_last=10
  [DBG] T1b kill(getpid(),SIGUSR1) ret=0 g_sig_usr1=1 g_sig_last=10
  ```
- POSIX 自测总结：`PASS=192 FAIL=0`（修复前 `PASS=189 FAIL=3`）——3 个失败用例全部转 PASS。
- 诊断打印已还原，`posixtest.c` 无 `[DBG]` 残留；子模块保持原始版本。

## 五、与 curl 卡死的关系 / 下一步

- **recv 无限阻塞**（curl 卡死主因）已由 step89 的 30s 有界超时（`net_rpc`）修复并提交（`56460ee`）。
- **信号号位撞车**（本步）破坏了 `raise()` 与 Shell 的 TTY 排空路径，是底层确定性缺陷；修复后 `raise()` 恢复、POSIX 兼容层信号语义完整。
- 完成后建议用户在图形 QEMU 重新 `exec bin/curl https://bilibili.com`：
  - 现在**不会再永久挂起**（30s 超时兜底）；若等不到数据会在约 30s 后返回明确错误并回到 shell；
  - 若成功取回页面，HTML 经 TTY 管道由 shell 渲染到终端。
- **验证盲区说明**：headless 串口只能看到 `u_print` 直接打印（自测框架），而 curl 二进制自身的 stdout（HTTP 响应体/错误）走 fd 1 → **TTY 管道**，只在交互式 shell 被排出，故 headless 无法判读 curl 的真实 HTTP 结局。bilibili 这种真实外网端点能否连通，取决于宿主 DNS（QEMU 代理 10.0.2.3）+ NAT 出网 + TLS 证书，需由 user 交互式运行确认。

## 六、仍存在的、有意为之的 ENOSYS 边界（非 bug，按优先级待补）

经全仓扫描，以下 POSIX/SukiNative 能力目前返回 `-ENOSYS`（POSIX 允许），属未实现子系统，**非本次撞号 bug**：
- `mmap` 文件映射（`sys_posix.c:1760` ENOSYS，需页缓存）—— 影响从磁盘 mmap 文件/共享库；当前 dltest 走的是已加载库路径故不受影响。
- `mremap` 仅支持等长（`sys_posix.c:1848`），`MREMAP_MAYMOVE` 增长返回 `-ENOSYS` —— realloc/IPC 缓冲增长会失败。
- `getitimer`/`setitimer`（`sys_posix.c:2062` ENOSYS）—— 需「定时器到期→发信号」子系统。
- `socketpair`/AF_UNIX（`sys_posix.c:2264` ENOTSUPP）—— 需内核态 Unix 域 socket 实现。
- SukiNative 130..149 部分 socket 原语（`sys_posix.c:2544` 附近个别 case 返回 ENOSYS）。

上述每一项都是独立子系统，需各自设计+自测，不能无验证地半成品填入（违背生产稳定要求）。建议按"mremap 增长 → itimer → 文件 mmap → AF_UNIX"优先级逐步补全，每项单独提交并附 headless 自测。
