# compapps/ — 第三方「程序 / 工具」子模块目录

本目录用于存放以 **git submodule** 方式引入的**外部程序 / 工具类**项目
（可执行程序、命令行工具、辅助构建器、独立应用等）。

## 约定（与项目规则一致）

- **外部库（library）** → 放在 **`lib/`**（如 `lib/lwip-2.2.1`、`lib/freetype-2.14.3`、
  `lib/mbedtls`、`lib/curl`）。
- **外部程序 / 工具（program / tool）** → 放在 **`compapps/`**（本目录）。
- 一律使用 `git submodule` 引入，**不再直接把上游源码拷贝入库**；新增后同步更新
  `.gitmodules` 与本 README / 主 `README.md` 的第三方组件说明。

## 目录状态

当前**尚无**程序类子模块，目录为空占位。首次引入程序类子模块示例：

```bash
git submodule add --depth 1 <repo-url> compapps/<name>
cd compapps/<name> && git fetch --depth 1 origin tag <tag> && git checkout <tag>
git add .gitmodules compapps/<name> && git commit
```

## 克隆 / 更新

```bash
git clone --recursive <SukiOS-url>      # 首次克隆一并拉取全部子模块
# 或已克隆后补齐：
git submodule update --init --recursive
```
