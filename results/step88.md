# step88 — 修复 shell `exec` 对磁盘程序路径的无效探测（消除 `[fs] read_at open FAIL fr=4`）

## 一、现象

在 SukiOS shell 中执行：

```
exec bin/curl https://bilibili.com
```

输出首行出现：

```
[fs] read_at open FAIL fr=4 path='bin/curl'
```

随后却正常 spawn：

```
[sched] user task 'curl.ska' pid=28 cpu=0 cr3=... entry=0x400000 (ELF, W^X)
[syscall] spawn: parent pid=10 -> child 'curl.ska' pid=28
```

即 `curl` 实际启动成功（pid=28），但第一行 FAIL 具有误导性。

## 二、根因

1. **磁盘程序的实际存放位置**：`Makefile` 把独立用户程序（`APP_PROGS` 含 `curl`）放入 FAT32 磁盘的 `/BIN/` 目录，文件名**大写 + `.SKA`** 后缀，即 `/BIN/CURL.SKA`（见 `Makefile` 注释“仅放入 FAT32 磁盘的 ::BIN/ 目录（文件名无 .elf 后缀）”以及 `kmain.c` 对 `::BIN/CURL.SKA` 的引用；shell 自身拉起服务也用 `/BIN/FONTSRV.SKA`、`/BIN/PCHFNT.SKA`）。
2. **shell exec 的旧解析逻辑**（`user/shell.c`，约 774 行）：
   - 先用 `argv[0]`（即 `bin/curl`）直接 `sys_task_spawn` → 内核 `exec_read_file` 经 FS_PORT 向 fs_server 发 `FS_MSG_READ_AT` → fs_server 打开 `bin/curl` 失败，`fr=4` 即 FatFs `FR_NO_FILE`（文件不存在），内核打印 `[fs] read_at open FAIL`。
   - 因 `bin/curl` 无 `.`，shell 退而补 `.ska` 重试 `bin/curl.ska` → fs_server 打开该路径，由于 FatFs **路径匹配大小写不敏感**（`bin`→`BIN`、`curl.ska`→`CURL.SKA`），命中 `/BIN/CURL.SKA` → 成功，curl 启动。
3. **结论**：FAIL 来自 shell 第一次对“不存在的裸路径 `bin/curl`”的探测，第二次才命中正确位置。该探测既多余又产生误导日志。

## 三、修复（`user/shell.c`）

重写 `exec` 的路径解析分支：

- 无扩展名、且不含 `/`（裸名，如 `curl`）：优先尝试 `/BIN/<name>.SKA`，其次 `<name>.ska`（当前目录），再次裸 `<name>`（根目录 legacy）；
- 无扩展名、但含 `/`（如 `bin/curl`）：按原样补 `.SKA` 得到 `bin/curl.SKA`（大小写不敏感命中 `/BIN/CURL.SKA`），避免先探测不存在的 `bin/curl`；
- 已带扩展名（如 `/BIN/CURL.SKA`、`/home/x/app.ska`）：按给定路径直接装载。

这样 `exec bin/curl` 第一次就命中 `bin/curl.SKA` → `/BIN/CURL.SKA`，不再产生 FAIL；同时 `exec curl`（不带 `bin/` 前缀）现在也能直接工作。

关键代码（`user/shell.c` exec 块）：

```c
int pid = -1;
const char *name = argv[0];
int has_dot = 0, has_slash = 0;
for (const char *q = name; *q; q++) {
    if (*q == '.')  has_dot = 1;
    if (*q == '/')  has_slash = 1;
}
if (!has_dot) {
    char probe[256]; int n = 0; const char *pp = name;
    if (!has_slash) { probe[n++]='/'; probe[n++]='B'; probe[n++]='I'; probe[n++]='N'; probe[n++]='/'; }
    while (*pp && n < (int)sizeof(probe)-6) probe[n++] = *pp++;
    probe[n++]='.'; probe[n++]='S'; probe[n++]='K'; probe[n++]='A'; probe[n]='\0';
    pid = sys_task_spawn(probe, argv, NULL);
    if (pid < 0) { /* 退回 <name>.ska（当前目录） */ ... }
    if (pid < 0 && !has_slash) pid = sys_task_spawn(name, argv, NULL); /* 根目录裸名 legacy */
} else {
    pid = sys_task_spawn(name, argv, NULL);
}
```

未触碰任何 submodule 文件；未改动 fs_server（其对真实打开失败打印 FAIL 是合理的诊断行为）。

## 四、验证

- `make disk` → `DISK_RC=0`，`make iso` → `ISO_RC=0`，全程 **0 warning / 0 error**（`shell.c` 改动未引入新告警）。
- 运行时验证需在 QEMU 内手动键入命令（headless 下无法注入键盘，详见下节），预期：
  - `exec bin/curl https://bilibili.com` 不再出现 `[fs] read_at open FAIL fr=4 path='bin/curl'` 行，curl 直接 spawn；
  - `exec curl https://bilibili.com` 亦可工作（新增能力）。
- 注意：`exec bin/curl` 仅解决“程序装载”告警；curl 真正取回 `https://bilibili.com` 还需**网络子系统可用**（net_server 已拉起、QEMU 配置网卡 `-netdev user`/`-device`、且完成 DHCP 获取地址）。若 spawn 后 curl 报连接/解析失败，那是网络层问题，与本次 FAIL 无关，需另行排查 net_server/lwIP。

## 五、结论

- 误导性 FAIL 的根因（exec 先探测不存在的裸路径）已修复，且顺带支持 `exec <name>` 直接装载 `/BIN` 下程序；
- 构建零告警；子模块保持原始版本（`git submodule status` 无 `+`）。
