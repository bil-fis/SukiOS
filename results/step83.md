# Step 83 — 以 git submodule 引入 mbedTLS/libcurl；lwip/freetype 切换为 submodule

> 日期：2026-09-12
> 承接：`results/step81.md`（libcurl 移植评估）、`results/step82.md`（网络 libc + 时间 + miniz）。
> 本轮目标（用户指令）：
> ① 用 **git submodule** 引入 mbedTLS（最新 LTS）与 libcurl（最新稳定版）；
> ② 记录「外部库一律用 submodule，库入 `lib/`、程序入 `compapps/`」的约定（记忆）；
> ③ 评估并（可行则）把 lwip、freetype 等切换为 submodule，然后更新 README。

结论：三项全部完成。mbedTLS/curl 已作为子模块就位；lwip/freetype 经**逐文件比对确认与上游 tag 一致**后成功切换为子模块，构建产物尺寸不变、QEMU 生产场景回归零 panic；README 新增 §5.1「git submodule 约定」并更新相关章节，NOTICE 补录三条许可。

---

## 1. 版本选型

| 库 | 选定版本 | 依据 |
| :-- | :-- | :-- |
| mbedTLS | **3.6.7**（tag `mbedtls-3.6.7`） | ① 3.6 为**长期支持 LTS 分支**（官方承诺至 2027-03）；② 4.x（4.0/4.1/4.2）为最新特性分支但**非 LTS**。用户要求「最新 LTS」→ 取 3.6 分支最新补丁版。 |
| libcurl | **8.22.0**（tag `curl-8_22_0`） | 上游最新稳定发布（`git ls-remote` 确认 8.22.0 为当前最高 `curl-8_*` 稳定 tag，无 9.x）。 |

> 版本经 `git ls-remote --tags` 实测确认（非凭记忆）。mbedTLS 另可参考其 LTS 说明（3.6 支持到至少 2027-03）。

---

## 2. 新增子模块

| 子模块路径 | 上游 | 固定提交 | 版本 tag |
| :-- | :-- | :-- | :-- |
| `lib/mbedtls` | `https://github.com/Mbed-TLS/mbedtls.git` | `068ff08` | `mbedtls-3.6.7` |
| `lib/curl` | `https://github.com/curl/curl.git` | `0134682` | `curl-8_22_0` |

方法（浅克隆 + 显式检出 tag，`.gitmodules` 记录 url/path）：

```bash
git submodule add --depth 1 https://github.com/Mbed-TLS/mbedtls.git lib/mbedtls
(cd lib/mbedtls && git fetch --depth 1 origin tag mbedtls-3.6.7 && git checkout mbedtls-3.6.7)
git submodule add --depth 1 https://github.com/curl/curl.git lib/curl
(cd lib/curl && git fetch --depth 1 origin tag curl-8_22_0 && git checkout curl-8_22_0)
```

两个子模块**尚未接入构建**（本轮只做引入），后续按 `results/step81.md` 方案集成（手写 `curl_config.h` + `libcurl.a`；mbedTLS 编译三库 + 用户态平台适配层）。

---

## 3. lwip / freetype 切换评估（可行 → 已切换）

### 3.1 评估方法（逐文件比对，非凭记忆）

原 `lib/lwip-2.2.1`、`lib/freetype-2.14.3` 为 **vendored**（源码副本直接入库，共 3039 个受控文件）。
为判断能否安全替换为上游子模块，先浅克隆对应上游 tag 并**逐文件比对**：

```bash
git clone --depth 1 --branch STABLE-2_2_1_RELEASE https://github.com/lwip-tcpip/lwip.git /tmp/lwip-up
git clone --depth 1 --branch VER-2-14-3        https://github.com/freetype/freetype.git /tmp/ft-up
diff -rq --strip-trailing-cr lib/lwip-2.2.1 /tmp/lwip-up --exclude=.git
diff -rq --strip-trailing-cr lib/freetype-2.14.3 /tmp/ft-up --exclude=.git
```

### 3.2 评估结果

- **lwip**：忽略行尾（CRLF/LF）差异后仅剩 **5 项**，且全部为上游 dotfile（`.gitattributes/.github/.gitignore/.vscode`）与生成的 `doc/doxygen/output/html`——**源码内容与上游 tag 完全一致**。
- **freetype**：忽略行尾后剩 **37 项**，均为上游 dotfile（`.gitignore/.gitlab-ci.yml/.gitmodules/.mailmap`）、**autotools 生成物**（`configure`/`aclocal.m4`/`config.guess`/`config.sub`/`ltmain.sh`/`ChangeLog`）与 Windows 工程文件——我们只编译 `src/<dirs>/*.c` 并自供配置头，**不使用**这些生成物。

结论：两者内容等同于上游 tag，**可行且安全**（项目侧所有裁剪/配置都在外部头中，不修改上游源码）。

### 3.3 执行切换

```bash
git rm -r lib/lwip-2.2.1 && rm -rf lib/lwip-2.2.1
git submodule add --depth 1 https://github.com/lwip-tcpip/lwip.git lib/lwip-2.2.1
(cd lib/lwip-2.2.1 && git fetch --depth 1 origin tag STABLE-2_2_1_RELEASE && git checkout STABLE-2_2_1_RELEASE)

git rm -r lib/freetype-2.14.3 && rm -rf lib/freetype-2.14.3
git submodule add --depth 1 https://github.com/freetype/freetype.git lib/freetype-2.14.3
(cd lib/freetype-2.14.3 && git fetch --depth 1 origin tag VER-2-14-3 && git checkout VER-2-14-3)
```

| 子模块路径 | 上游 | 固定提交 | 版本 tag |
| :-- | :-- | :-- | :-- |
| `lib/lwip-2.2.1` | `https://github.com/lwip-tcpip/lwip.git` | `77dcd25` | `STABLE-2_2_1_RELEASE` |
| `lib/freetype-2.14.3` | `https://github.com/freetype/freetype.git` | `0a0221a` | `VER-2-14-3` |

> **目录名保持不变**，故 `Makefile` 的 `LWIP_DIR := lib/lwip-2.2.1`、`FT_DIR := lib/freetype-2.14.3` 无需改动。
>
> **踩坑记录**：在 WSL/drvfs 上 `git submodule add` 偶发在注册阶段中断（`.gitmodules` 只写了 `path`、缺 `url`，报
> `fatal: please make sure that the .gitmodules file is in the working tree`）。此时子模块仓库已克隆、`.git/config` 也已登记 url，**只需手动补全 `.gitmodules` 的 `url` 行**，再 `git add .gitmodules <path>` 即可。已在本轮如实记录以便复现。

---

## 4. 验证

### 4.1 构建（子模块源码）

```bash
make iso disk
```
- 通过，无 error。
- **`build/kernel.ski` = 1 347 880 B、`build/SukiOS.iso` = 13 228 032 B，与切换前完全一致** —— 佐证上游 tag 与 vendored 内容在构建意义上等价。

### 4.2 QEMU 生产场景回归（子模块化后）

```bash
timeout 160 make run-headless QEMU_SERIAL="-serial file:/tmp/suki4.log"
grep -inE "panic|#GP|#PF|page fault" /tmp/suki4.log     # 无
```
结果（与切换前一致）：
```
[fs] mounted FAT32 (FatFs)
[fs] self-test ALL PASS
[boot] display-server ready (g_display_active=1, waited=0 yield rounds)
[libc-test] miniz roundtrip OK (115 -> 101 bytes)
[libc-test] PASS=11 FAIL=0  ALL OK
[nettest] POSIX UDP round-trip (with reply): PASS
[nettest] libc-net API: PASS=4 FAIL=0  ALL OK
=== POSIX test summary: PASS=192 FAIL=0 ===
```
零 panic。

---

## 5. 约定（已写入长期记忆 + README §5.1）

- **外部第三方项目一律用 `git submodule` 引入**，不再把上游源码直接拷贝入库。
- **库（library）→ `lib/`**；**程序 / 工具（program / tool）→ `compapps/`**。
- 新增子模块后同步更新 `.gitmodules` 与 README/NOTICE。
- 克隆：`git clone --recursive`，或 `git submodule update --init --recursive`。

本轮新增 `compapps/` 目录（内含 `compapps/README.md` 说明约定，当前为空占位）。

---

## 6. 文档更新

### `README.md`
- §4.2 构建命令：新增「第 0 步」`git submodule update --init --recursive` 及说明块。
- **新增 §5.1「第三方依赖：git submodule 约定」**：类别/位置表（`lib/` vs `compapps/`）、当前子模块登记表（上游/固定版本/用途）、使用命令，及 lwip/freetype 由 vendored 切换为 submodule 的说明。
- §5 项目结构：`lib/` 改为标注 4 个子模块（含 tag 与许可证），新增 `compapps/` 行。
- §6.1 贡献环境：`git clone --recursive` + 子模块说明。
- §6.5 第三方组件边界：submodule 不随主仓库提交、项目侧经外部头适配、新库一律 submodule。
- §3 未实现：新增「HTTP/HTTPS 客户端（libcurl + mbedTLS，已备料未集成）」条目。
- §7 致谢、§8 许可表：新增 mbedTLS、libcurl，并**修正 lwIP 许可证标注 BSD-2-Clause → BSD-3-Clause**。

### `NOTICE`
- **补录 lwIP（原先缺失）**：BSD-3-Clause 全文摘要 + submodule/外部配置说明。
- 新增 **mbedTLS**（Apache-2.0 或 GPL-2.0-or-later 双许可）与 **libcurl**（curl 许可）条目。

### `compapps/README.md`（新增）
- 说明程序类子模块归属与引入示例。

---

## 7. 提交记录

| 提交 | 内容 |
| :-- | :-- |
| `f7ad74e` | `build(submodule): 以 git submodule 引入 mbedTLS(LTS 3.6.7) 与 libcurl(8.22.0)` |
| `9402f7e` | `build(submodule): lwip/freetype 切换为 git submodule;新增 compapps/ 与 README/NOTICE 说明` |

（均未 `push`，除非维护者明确要求。）

## 8. 当前子模块总览

```bash
$ git submodule status
 01346829096c61b372692f6dc43ffa778c6caccd lib/curl (curl-8_22_0)
 0a0221a1347e2f1e07c395263540026e9a0aa7c7 lib/freetype-2.14.3 (VER-2-14-3)
 77dcd25a72509eb83f72b033d219b1d40cd8eb95 lib/lwip-2.2.1 (STABLE-2_2_1_RELEASE)
 068ff080b369adfac81509f9b57b2afabaf82dc5 lib/mbedtls (mbedtls-3.6.7)
```

## 9. 后续

- 集成 mbedTLS（编译 `library/*.c` 为静态库 + 用户态平台适配：熵源 RDRAND/软 PRNG、timing 经 `gettimeofday`）。
- 集成 libcurl（手写 `curl_config.h` + `libcurl.a`，开启 HTTP、以 mbedTLS 提供 HTTPS），复用 step82 已备好的 socket/DNS/select/fcntl/时间/miniz 底座。
