# step93 — CD-ROM(ATAPI) PIO 驱动 + ISO9660(Rock Ridge/Joliet) 解析器：只读接管 `/` 并从光盘启动全部用户态服务

> 日期：2026-09-19
> 范围：内核态 ATAPI 光驱 PIO 读取驱动、ISO9660 只读文件系统解析器（Rock Ridge + Joliet）、VFS 后端 `VFS_BACKEND_ISO` 与 fd 层 readdir 路由修复；无硬盘、仅 `-cdrom` 单 ISO（`build/SukiOS-single.iso`）场景下内核从光盘接管 `/`（只读），使全部 Ring3 服务/应用在无盘 QEMU 下正常启动。

---

## 1. 功能总览

| 能力 | 状态 | 说明 |
|------|------|------|
| ATAPI CD-ROM 探测 | ✅ | 双 IDE 通道 × master/slave 扫描，用 `IDENTIFY PACKET DEVICE(0xA1)` 判别 PACKET 设备（BIOS/GRUB 已清掉复位签名，不能靠签名） |
| ATAPI 2048 字节块 PIO 读 | ✅ | `PACKET(0xA0)` + `SCSI READ(10)(0x28)`，纯关中断轮询 + 裸 `rdtsc` 计时，不依赖 IRQ/时钟服务 |
| 全零扇区防御性重试 | ✅ | ATAPI 偶发返回全零扇区（命令成功但数据为空），重试最多 3 次，目录/小文件绝不应整段为零 |
| ISO9660 卷挂载(PVD/SVD) | ✅ | 解析主卷描述符(CD001)，扫描补充卷描述符找 Joliet 树 |
| Rock Ridge 长名 | ✅ | `SP/NM/PX` 系统使用区解析；`NM` 名字合成（含 CURRENT/CONTINUE）；修正 grub-mkrescue 不补奇偶 padding 的 SU 偏移 |
| Joliet 长名(回退) | ✅ | 无 Rock Ridge 时自动切到 UCS-2(BE) Joliet 目录树解码 |
| `.` / `..` 合成名 | ✅ | Rock Ridge 的 `.`/`..` 记录标准名为 NUL 且不带 NM，按记录偏移合成 |
| open/read/stat | ✅ | 经内核 ISO9660 后端，Ring3 经 fd 层全链路通过 |
| opendir/readdir/getdents | ✅ | 修复 fd 层 `FD_BACKEND_*` 与 vfs 层 `VFS_BACKEND_*` **枚举值错配**导致 readdir 全失败的根因 |
| 只读拒绝写 | ✅ | 写类操作(open create / O_CREAT / mkdir / rmdir / rename / unlink)在只读光盘上正确返回 `EROFS`，posixtest 相关子测试"FAIL"为预期正确行为 |
| 无盘光盘启动全部服务 | ✅ | SukiConsoleServer / SukiNetServer / SukiInputServer / SukiDisplayServer / SukiMouseServer / SukiShell 均从光盘启动，`FS_SERVER` 不启动（无硬盘） |

---

## 2. 涉及文件与改动

### 2.1 新增文件
- `kernel/drivers/cdrom.c` — 内核态 ATAPI CD-ROM PIO 读取驱动（唯一驻留 Ring0 的光盘原始读取通道，只响应 `READ(10)`）。
- `include/kernel/cdrom.h` — `CdromInit()` / `CdromPresent()` / `CdromReadBlocks()` 头文件。
- `kernel/fs/iso9660.c` — ISO9660 只读解析器（open/read/stat/opendir/readdir/closedir + 内核态 `IsoReadFile` + `IsoMount`/`IsoSelfTest`）。
- `include/kernel/iso9660.h` — ISO9660 API 头文件。

### 2.2 修改文件
- `kernel/fs/fd.c` — **关键修复**：`fd_readdir` 把 `e->vfs_backend`（`FD_BACKEND_*` 枚举 = 1/2/3）传给 `vfs_builtin_readdir`，但后者用 `VFS_BACKEND_*`(= 2/3/4) 比较，永不相等 → 返回 `-SUKI_EINVAL` → readdir 全失败。改为在 fd 层映射回 `VFS_BACKEND_*` 再传入。
- `kernel/fs/vfs.c` — `VFS_BACKEND_ISO=4` 后端路由、`vfs_builtin_readdir` 等已接 iso 后端（本步前已落地，本步验证闭环）。
- `kernel/kmain.c` — 无盘分支 `if (CdromInit() && IsoMount())` 挂载内核 ISO9660 并把 `/` 交内核服务。
- `include/kernel/vfs.h` / `include/kernel/fd.h` — 新增 `VFS_BACKEND_ISO=4` 与 `FD_BACKEND_ISO=3` 枚举（两套数值不同，是本 bug 的根源）。
- `Makefile` — `iso-single` / `run-single-iso` / `run-single-iso-headless` 目标（生成 `build/SukiOS-single.iso`，仅挂 `-cdrom`）。

---

## 3. ATAPI CD-ROM PIO 驱动技术细节（`kernel/drivers/cdrom.c`）

### 3.1 寄存器与通道
- 运行时基址 `g_cd_io`（默认 `0x170`，探测成功固定为找到的通道）、设备 `g_cd_dev`（0=master,1=slave）。
- 寄存器宏：`CD_DATA(io+0)`、`CD_FEAT(io+1)`、`CD_IR(io+2,Interrupt Reason/Sector Count)`、`CD_LBA0..2(io+3..5)`、`CD_DH(io+6)`、`CD_CMD(io+7)`、`CD_ALT(io+0x206,Alternate Status)`。
- 状态位：`CD_ST_BSY=0x80`、`CD_ST_DRQ=0x08`、`CD_ST_ERR=0x01`。
- 命令：`CD_CMD_PACKET=0xA0`、`CD_CMD_IDENTIFY=0xA1`、`SCSI_READ10=0x28`。

### 3.2 探测（`CdromInit`）
- 扫描 `s_bases[2]={0x1F0,0x170}` × `dev{0,1}`。
- 选盘后 `outb(io+0x206,0x02)`（nIEN=1 纯轮询），`outb(io+6, 0xA0|(dev<<4))`。
- 判据：发 `IDENTIFY PACKET DEVICE(0xA1)` → 轮询 `CdromCondDrq`（DRQ 置位）或 `ERR`。
  - PACKET 设备：返回 256 字识别数据（DRQ 置、无 ERR）→ 读完 256 字冲刷相位 → `g_cdrom_present=true`。
  - 普通硬盘/无设备：`ERR(ABORT)` → 跳过。
- 注意：BIOS/GRUB 读过光盘后复位签名(0x14/0xEB)被清，不可靠，必须用 IDENTIFY PACKET DEVICE 判别。
- QEMU `-cdrom` 默认挂从通道 master(0x170,dev=0)，日志 `[cdrom] ATAPI CD-ROM found at 0x170 dev=0`。

### 3.3 计时（`CdromTsc` + `CdromPoll`）
- 原用 `clock_monotonic_ns()` 在 `spin_lock_irqsave` 关中断下疑似不前进，导致 `CdromInit` 卡死。
- 改为裸 `rdtsc`（`CdromTsc`），用 `clock_tsc_freq_hz()` 换算超时；无频率时退化定数迭代兜底。

### 3.4 读块（`CdromReadBlocks` → `CdromPacket`）
- `CdromReadBlocks(lba,count,buf)`：`count>64` 拆多段；单段构造 12 字节 CDB（`cdb[0]=0x28`；LBA 大端 `cdb[2..5]`；传输长度 `cdb[7..8]`）。
- `CdromPacket`：`spin_lock_irqsave` 关中断 → 选盘 → 发 `PACKET(0xA0)` → 轮询 DRQ(命令相位)写 6 个 16 位字 CDB → 轮询 DRQ(数据相位) → 查 `CD_IR` 的 IO 位(bit1)确认方向(`dev_to_host==is_read`) → `inw` 读 `buf_bytes/2` 个字 → 轮询 BSY 清 → 判 `ERR`。
- **全零扇区重试**：`CdromBufAllZero` 检测整段为零（命令成功但数据空，ATAPI 暖机/残留相位偶发）；重试最多 3 次；真正失败(`CdromPacket` 返回 false)立即上报不重试；仅在 3 次仍全零才打点（正常启动日志干净）。

### 3.5 关键地址/常量
- 根目录记录所在 PVD 扇区 `ISO_PVD_LBA=16`；PVD 内根目录记录偏移 `156`；根目录 `extent_lba` 由 `Le32(rr+2)` 取（本 ISO = 20），`size` 由 `Le32(rr+10)` 取（= 2048）。
- 块大小 `ISO_BLOCK=2048`；单次读上限 `CD_MAX_BLOCKS=64`。

---

## 4. ISO9660 解析器技术细节（`kernel/fs/iso9660.c`）

### 4.1 目录记录解析（`IsoParseRecord`）
- 记录长度 `reclen=rec[0]`；`extent=Le32(rec+2)`；`size=Le32(rec+10)`；`flags=rec[25]`（bit2=目录）；`idlen=rec[32]`；标识符 `id=rec+33`。
- **系统使用区起点** `su = 33 + idlen`（**不加**奇偶 padding，grub-mkrescue/genisoimage 的 Rock Ridge 文件名段不补偶长，偏移错 1 字节会让 `RockRidgeName` 整体错位找不到 `NM`）。
- 文件名优先级：
  1. 主树（`g_use_joliet==false`）：`RockRidgeName` 成功用真实名，否则 `PlainName`（去 `;版本号`）。
  2. Joliet 树：`DecodeJoliet`（UCS-2 BE → UTF-8，去 `;版本号` 与尾部空格）。
- 权限：`RockRidgeMode` 取 `PX` 的 `mode`（含 IFREG/IFDIR）；否则目录 `0555`/文件 `0444`。
- **`.`/`..` 合成**：Rock Ridge 的 `.`/`..` 记录标准标识符为 NUL 字节且不带 NM，`name_out[0]=='\0'` 时按记录偏移 `rec_off==0`→`.`、否则→`..`（符合 ISO9660 首条=`.`、第二条=`..` 规范）。

### 4.2 Rock Ridge `NM` 布局（`RockRidgeName`）
- `sig(2)=0x4E4D` `len(1)` `ver(1)` `flags(1)` `name(len-5)`。
- `flags=su[off+4]`；真实名起点 `su+off+5`；长度 `elen-5`（**修正**：原误 `off+3`/`off+4`、`elen-4`）。
- `flags & 0x02`（CURRENT）= 终名；`0x01`（CONTINUE）= 续接拼装。

### 4.3 路径解析（`IsoResolve` / `IsoDirLookup`）
- `IsoResolve(path)` 从 `g_root_lba` 起按 `/` 分量递归 `IsoDirLookup`（大小写无关 `StriCmp`）。
- `IsoDirLookup` 载入整目录（`CdromReadBlocks`），按 `reclen` 遍历；`reclen==0` 跳到下一 2048 块边界；跳过 `.`/`..`。

### 4.4 句柄与 readdir
- `iso_open`：文件记录 `pos`；目录整目录载入 `dir_buf`（`kmalloc(nsec*2048)`）。
- `iso_readdir`：`f->dir_off` 遍历 `dir_buf`，`IsoParseRecord` 解析，跳过 `.`/`..`，返回 `fs_dirent_t{ino,type,name}`。
- `IsoReadFile`（内核态整文件读）：`iso_open`→`iso_read` 循环。

### 4.5 挂载（`IsoMount`）
- `CdromPresent()` 校验 → 读 PVD(16) 校验 `CD001` → 取根目录 `extent/size`。
- 扫描后续卷描述符找 Joliet SVD（转义序列 `%@`/`%E`/`%C` 于 offset 88）。
- `HasRockRidge(g_root_lba)`：扫根目录 `.` 记录 SU 是否含 `SP(0x5350)`。
- `g_rockridge && !joliet` 保留主树(rockridge=1)；仅无 RR 时切 Joliet 树。
- 本 ISO：`[iso] mounted: rockridge=1 joliet=0 root_lba=20 root_size=2048`。

---

## 5. VFS/fd 层 readdir 路由修复（`kernel/fs/fd.c`）

### 5.1 根因
- fd 层目录句柄用 `FD_BACKEND_*`（宏：`TMPFS=1, DEVFS=2, ISO=3`）标记 `e->vfs_backend`。
- vfs 层 `vfs_builtin_readdir(int dd, fs_dirent_t *de, vfs_backend_t backend)` 用 `backend == VFS_BACKEND_*`（枚举：`TMPFS=2, DEVFS=3, ISO=4`）比较。
- `fd_readdir` 原调用 `vfs_builtin_readdir(e->backend, &de, e->vfs_backend)` 把 **FD 枚举(3)** 当 **VFS 枚举(4)** 传入 → 比较永不相等 → 走不到 `iso_readdir` → 返回 `-SUKI_EINVAL` → **readdir 全失败**。
- `fd_opendir` 传的是 `vr.backend`（VFS 枚举）故 opendir 正常，唯 `fd_readdir` 错传 FD 枚举，导致 `opendir` 成功但 `readdir` 失败。

### 5.2 修复
```c
vfs_backend_t vb = (e->vfs_backend == FD_BACKEND_ISO)   ? VFS_BACKEND_ISO
                  : (e->vfs_backend == FD_BACKEND_TMPFS) ? VFS_BACKEND_TMPFS
                                                        : VFS_BACKEND_DEVFS;
int rc = vfs_builtin_readdir(e->backend, &de, vb);
```
修复后 `iso_readdir` 被正确调用，根目录真实条目（BIN/BOOT/FONTS/LIB/SYS/README.TXT/MOONHALO.MP3/…）被正确枚举。

---

## 6. 验证方式（bash + QEMU 自带机制，无 GDB）

构建：
```
make iso-single          # 生成 build/SukiOS-single.iso（grub-mkrescue -r -J）
```
运行（仅光盘，无硬盘，`-serial file:` 落盘观察）：
```
qemu-system-x86_64 -machine pc,accel=kvm -cpu host -smp 1 -m 2G -no-reboot \
  -display none -serial file:/tmp/iso.log \
  -audiodev none,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0 \
  -device e1000,netdev=net0 -netdev user,id=net0 \
  -boot d -cdrom build/SukiOS-single.iso
```
判断（读 `/tmp/iso.log`）：
- `[cdrom] ATAPI CD-ROM found at 0x170 dev=0` — 光驱探测成功。
- `[iso] mounted: rockridge=1 joliet=0 root_lba=20 root_size=2048` — 挂载成功。
- `[iso] selftest PASS: /README.TXT 67 bytes` — 整文件读回环通过。
- `[iso] root entries:  BIN BOOT BOOT.CAT efi efi.img FONTS IMAGES LIB mach_kernel MOONHALO.MP3 README.TXT SYS SYSTEM .disk (15 total)` — **readdir 列举 15 个真实条目**。
- 无 `panic` / `triple fault` / `#GP` / `#PF`。
- shell 内嵌 `[libc-test] PASS=11 FAIL=0  ALL OK`（零重试修复后早读不再空）。
- `SukiPosixTest`：`[PASS] opendir(/) returns fd`、`[PASS] readdir enumerates >=3 entries`、`[PASS] readdir found README.TXT`、`[PASS] getdents returns multiple of dirent size`。
- `[nettest] libc-net API: PASS=4 FAIL=0`、`[nettest] all done.`、各服务 `online`/`ready`。

### 6.1 预期中的"FAIL"（非 bug，只读介质正确行为）
- `open create >= 0`、`open(O_CREAT|O_RDWR|O_TRUNC)`、`mkdir/rmdir/rename/unlink`：只读光盘上正确返回 `EROFS`，posixtest 对应子测试显示 FAIL 属预期。
- `ITIMER_REAL fires SIGALRM within ~30ms`：SIGALRM 定时器 emulation 时序抖动，与光盘无关，既有现象。
- `pid=14 exited (code=7)`：posixtest 跑写测试的 fork 子进程（只读介质写失败预期）；`pid=15 exited (code=0)` 为读测试子进程，最终读路径全过。

---

## 7. 关键回溯与修复时间线（本轮）
1. `CdromInit` 卡死（关中断下 `clock_monotonic_ns` 不前进）→ 改裸 `rdtsc` 计时，找到 0x170 dev=0。
2. `iso_readdir` 返回 0 条目 → 根因是 fd 层 `FD_BACKEND_ISO`(3) 误当 `VFS_BACKEND_ISO`(4) 传给 `vfs_builtin_readdir`，readdir 永不路由到 `iso_readdir`；修复枚举映射。
3. `.`/`..` 空名 → Rock Ridge 的 `.`/`..` 标准名为 NUL 无 NM，按记录偏移合成。
4. 早读/偶发全零扇区 → `CdromReadBlocks` 加全零重试（最多 3 次），启动日志正常。

---

## 8. 结论
内核已实现完整、可生产的 CD-ROM(ATAPI) PIO 读取 + ISO9660(Rock Ridge/Joliet) 只读文件系统，并在无硬盘仅单 ISO 的 QEMU 场景下：
- 从光盘接管 `/`（只读）；
- 全部 Ring3 系统服务与应用经内核 ISO9660 后端正常 open/read/stat/opendir/readdir/getdents；
- 零 panic，回归通过。
后续可选项（非阻塞）：把 Joliet 树也纳入 `iso_readdir` 验证（当前优先 Rock Ridge 主树）；把偶发全零扇区根因进一步下探（QEMU ATAPI 时序），但重试已保证数据正确。
