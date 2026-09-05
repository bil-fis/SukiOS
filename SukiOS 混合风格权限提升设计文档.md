# SukiOS 混合风格权限提升设计文档 v1.0

---

## 一、设计目标与核心原则

### 1.1 设计目标

1. **混合风格**：融合 Windows UAC 的“完整性级别”概念与 macOS Authorization Services 的“按需授权”模式。
2. **无 GUI 环境友好**：在窗口管理器就绪前，支持通过 Shell/TUI 进行特权确认。
3. **与现有架构兼容**：充分利用已有的 Mach 端口权能机制，不推倒重来。
4. **可审计**：所有特权操作可追溯。
5. **渐进式实施**：可分阶段部署，GUI 就绪后可平滑升级。
6. **调试角色**：提供最高权限的调试用户，用于开发和故障诊断，且生产环境默认禁用。

### 1.2 核心原则

- **最小权限**：进程默认只有执行基本任务所需的最小权限。
- **显式授权**：任何特权操作必须经过用户显式确认或系统策略授权。
- **权限降级**：特权操作完成后，进程应主动降回普通权限。
- **可配置策略**：系统管理员可配置文件定义哪些操作需要授权、哪些可免密执行。
- **调试隔离**：调试角色仅在特定启动参数下启用，不影响正常系统的安全模型。

### 1.3 Shell 输出语言

由于 SukiOS Shell 当前不支持中文显示，所有交互式提示（如授权对话框、错误信息）将使用**英文**输出。本文档中的示例代码均遵循此约定。

---

## 二、核心概念与数据结构

### 2.1 用户账户（User Account）

```c
// include/kernel/security/user.h

#define SUKI_USER_NAME_MAX 64

typedef enum suki_user_type {
    SUKI_USER_SYSTEM = 0,      // 系统账户（内核/服务）
    SUKI_USER_ADMIN,           // 管理员账户
    SUKI_USER_STANDARD,        // 标准用户
    SUKI_USER_GUEST,           // 访客用户
    SUKI_USER_DEBUG,           // 调试用户（仅在 debug 模式下可用）
} suki_user_type_t;

typedef struct suki_user {
    uint32_t        uid;
    uint32_t        gid;
    char            name[SUKI_USER_NAME_MAX];
    uint8_t         password_hash[64];   // SHA-512
    suki_user_type_t type;
    uint64_t        flags;               // 锁定/禁用/过期等
    uint64_t        home_dir_inode;
    uint32_t        failed_attempts;
    uint64_t        locked_until;
} suki_user_t;
```

### 2.2 进程安全令牌（Security Token）

```c
// include/kernel/security/token.h

/* 完整性级别（Windows UAC 风格） */
#define SUKI_IL_UNTRUSTED    0    // 不可信（最低）
#define SUKI_IL_LOW          1    // 低完整性
#define SUKI_IL_MEDIUM       2    // 中完整性（标准用户默认）
#define SUKI_IL_HIGH         3    // 高完整性（管理员）
#define SUKI_IL_SYSTEM       4    // 系统完整性（最高，通常为内核/服务）
#define SUKI_IL_DEBUG        5    // 调试完整性（仅在 debug 模式可用，拥有全部权限）

typedef struct suki_security_token {
    uint64_t        token_id;
    
    // 身份信息
    suki_uid_t      uid;
    suki_uid_t      euid;
    suki_uid_t      saved_uid;
    suki_gid_t      gid;
    suki_gid_t      egid;
    suki_gid_t      saved_gid;
    uint32_t        supplementary_gid_count;
    suki_gid_t      supplementary_gids[32];
    
    // 能力位图（POSIX capabilities 风格扩展）
    uint64_t        capabilities[2];    // 128 位
    
    // 完整性级别（Windows UAC 风格）
    uint32_t        integrity_level;    // 0-5
    
    // 授权缓存（macOS 风格）
    uint64_t        auth_cache[8];      // 最近授权的操作哈希
    
    // 沙盒限制
    uint64_t        sandbox_flags;
    
    // 审计
    uint64_t        audit_session_id;
    uint64_t        created_at;
} suki_security_token_t;
```

### 2.3 授权令牌（Authorization Token）—— macOS 风格

```c
// include/kernel/security/auth.h

typedef uint64_t suki_auth_token_t;

#define SUKI_AUTH_TOKEN_INVALID ((suki_auth_token_t)-1)

typedef enum suki_auth_status {
    SUKI_AUTH_PENDING,      // 等待用户确认
    SUKI_AUTH_GRANTED,      // 已授权
    SUKI_AUTH_DENIED,       // 被拒绝
    SUKI_AUTH_EXPIRED,      // 已过期
    SUKI_AUTH_REVOKED,      // 已撤销
} suki_auth_status_t;

typedef struct suki_auth_request {
    uint32_t        pid;                // 请求进程
    uint32_t        uid;                // 请求用户
    char            operation[64];      // "install_package", "modify_system"
    char            target[256];        // 目标资源路径
    uint64_t        required_caps;      // 所需能力位
    uint32_t        min_il;             // 所需最低完整性级别
    uint64_t        timestamp;
    uint32_t        timeout_ms;         // 超时时间
    uint32_t        flags;              // SUKI_AUTH_FLAG_*
} suki_auth_request_t;

typedef struct suki_auth_token {
    uint64_t        token_id;
    uint32_t        pid;                // 拥有者进程
    uint32_t        uid;                // 授权用户
    uint64_t        granted_caps;       // 授予的能力
    uint32_t        granted_il;         // 授予的完整性级别
    uint64_t        expiry;             // 过期时间
    suki_auth_status_t status;
    uint32_t        ref_count;          // 引用计数（可通过端口传递）
} suki_auth_token_t;
```

### 2.4 权限策略配置（doas/Polkit 风格）

```c
// /etc/suki/auth.conf —— 配置文件格式

# 语法：permit|deny [nopass] [as <user>] <command_pattern> [args...]

# 管理员组可以免密执行任何命令
permit nopass :wheel

# 标准用户安装软件需要授权
permit :users /system/bin/pkg install

# 特定用户可执行系统更新（免密）
permit nopass alice /system/bin/sysupdate

# 调试用户自动拥有全部权限（但仅在 debug 模式生效）
permit nopass :debug

# 默认拒绝
deny * *
```

### 2.5 权限决策结果

```c
typedef enum suki_decision {
    SUKI_DECISION_ALLOW = 0,      // 允许
    SUKI_DECISION_DENY,           // 拒绝
    SUKI_DECISION_PROMPT,         // 需要用户确认（GUI/Shell 弹窗）
    SUKI_DECISION_ELEVATE,        // 自动提升（配置允许）
} suki_decision_t;
```

---

## 三、调试角色（Debug User）详细设计

### 3.1 定义

- **调试用户**：一个特殊账户，在系统启动时若检测到 `suki.debug=1` 内核参数，则自动启用。
- **UID**：固定为 `0`（与 root 相同），但 `type = SUKI_USER_DEBUG`，`integrity_level = SUKI_IL_DEBUG (5)`。
- **权限**：拥有全部能力位（所有 128 位均为 1），可执行任何操作，不受策略限制。
- **生命周期**：仅在 `debug` 模式活跃；正常启动时该账户不存在或不可登录。

### 3.2 启用条件

内核启动参数 `suki.debug=1` 或 `debug=1` 时，系统初始化阶段创建调试用户，并自动以该用户身份启动一个特权 Shell（或允许 `su` 切换到该用户）。

### 3.3 权限检查中的快速通道

在权限检查函数中，优先判断令牌的 `integrity_level` 是否等于 `SUKI_IL_DEBUG`：

```c
suki_decision_t suki_check_permission(
    suki_security_token_t *token,
    suki_resource_type_t   res_type,
    uint64_t               res_id,
    uint32_t               requested_access,
    suki_auth_token_t     *out_token
) {
    // 1. 无效令牌 -> 拒绝
    if (!token) return SUKI_DECISION_DENY;
    
    // ★ 调试角色快速通道：无条件允许，并记录审计日志
    if (token->integrity_level == SUKI_IL_DEBUG) {
        suki_log_audit(SUKI_AUDIT_DEBUG_ALLOW, token->token_id, res_type, res_id);
        return SUKI_DECISION_ALLOW;
    }
    
    // 2. 系统完整性级别（IL_SYSTEM）-> 允许（但调试级别更高，已先捕获）
    if (token->integrity_level >= SUKI_IL_SYSTEM) {
        return SUKI_DECISION_ALLOW;
    }
    
    // 3. 检查能力位图...
    // 4. 检查策略...
    // 5. 检查授权缓存...
    // 6. 需要用户确认...
}
```

### 3.4 审计与安全注意事项

- 所有调试操作都记录审计日志，便于问题复现和安全审计。
- 生产环境禁止启用 `suki.debug=1`，可通过编译时或启动时校验（如签名验证）强制禁止。
- 调试角色的密码在文档中注明为调试用途，建议在发布版本中强制更改或禁用。

---

## 四、权限检查流程（完整）

### 4.1 流程图

```
进程发起特权操作
        ↓
内核检查令牌
        ├─ 是调试角色？ → 允许（审计）
        ├─ 完整性级别 ≥ IL_SYSTEM？ → 允许
        ├─ 拥有所需能力？ → 允许
        ├─ 策略允许（nopass）？ → 允许
        ├─ 授权缓存命中？ → 允许
        ├─ 策略要求授权？ → 跳转至授权流程
        └─ 其他 → 拒绝
```

### 4.2 授权流程（完整时序）

```
应用发起特权操作
        ↓
内核检查权限不足 → 返回 SUKI_DECISION_PROMPT
        ↓
内核向 AUTH_PORT 发送授权请求（suki_auth_request_t）
        ↓
AUTH_SERVER 接收请求
        ↓
┌───────────────────────────────────────┐
│  判断当前环境                         │
├───────────────────────────────────────┤
│  有 GUI (DISPLAY_SERVER 运行中)      │ → 弹出图形授权对话框（英文界面）
│  无 GUI (仅 Shell)                   │ → 在 Shell 中打印英文授权请求
└───────────────────────────────────────┘
        ↓
用户确认（输入密码 / 点击允许）
        ↓
AUTH_SERVER 签发授权令牌（suki_auth_token_t）
        ↓
令牌通过 Mach 端口返回给应用（端口权能传递）
        ↓
应用使用令牌执行特权操作
        ↓
操作完成后令牌自动过期 / 应用主动撤销
```

### 4.3 无 GUI 环境的 Shell 授权交互（英文）

```bash
$ pkg install servo-browser

[SUKIOS] This operation requires administrator privileges.
Operation: install_package
Target: /system/apps/
Requesting process: shell (PID=42)
User: alice (standard)

Please select:
  [1] Enter administrator password
  [2] Use cached authorization (if available)
  [3] Cancel
> 1

Password: ********
[SUKIOS] Authorization granted (expires in 5 minutes).
Installing servo-browser...
```

### 4.4 授权缓存与超时

- 授权令牌默认有效期 5 分钟，可配置。
- 用户可勾选“记住此选择”以延长缓存时间（最长 1 小时）。
- 超时后令牌自动失效，需重新授权。

---

## 五、特权辅助工具模式（借鉴 macOS SMJobBless）

### 5.1 设计思路

参考 macOS 的 `SMJobBless` 机制——应用嵌入一个经过验证的特权辅助工具，该工具以 root 身份运行，执行高权限操作。

```
┌─────────────────────────────────────────────────────────────┐
│  普通应用 (Ring3, 标准用户)                               │
│  ├── 需要执行特权操作                                     │
│  ├── 通过 IPC 向特权辅助工具发送请求                      │
│  └── 请求经 AUTH_SERVER 授权验证                         │
├─────────────────────────────────────────────────────────────┤
│  特权辅助工具 (Ring3, root 身份运行)                     │
│  ├── 由 launchd 风格的 service manager 启动              │
│  ├── 通过 IPC 接收普通应用的请求                         │
│  └── 执行高权限操作（文件修改、驱动安装等）              │
└─────────────────────────────────────────────────────────────┘
```

### 5.2 实现方案

```c
// user/privileged_helper/helper.c

int main(int argc, char **argv) {
    // 1. 验证调用者身份（通过 Mach 端口权能）
    uint32_t caller_pid = mach_msg_get_sender_pid();
    suki_security_token_t *caller_token = task_lookup(caller_pid)->token;
    
    if (!suki_has_permission(caller_token, SUKI_CAP_USE_HELPER)) {
        return -EACCES;
    }
    
    // 2. 进入服务循环
    port_set_owner(HELPER_PORT, sched_current());
    for (;;) {
        helper_request_t req;
        if (ipc_recv_kernel(HELPER_PORT, &req, sizeof(req), NULL, true) 
            != MACH_MSG_SUCCESS) {
            continue;
        }
        
        // 3. 验证请求是否已获得 AUTH_SERVER 授权
        if (!suki_auth_validate(req.auth_token, req.operation)) {
            suki_send_response(req.reply_port, -EACCES);
            continue;
        }
        
        // 4. 执行特权操作
        int result = suki_execute_privileged(req.operation, req.args);
        suki_send_response(req.reply_port, result);
    }
}
```

---

## 六、开源参考实现

### 6.1 授权服务框架

| 项目 | 描述 | 许可证 | 参考价值 |
| :--- | :--- | :--- | :--- |
| **Polkit (PolicyKit)** | Linux 桌面环境的授权框架 | GPL/LGPL | 策略驱动的授权模型 |
| **OpenDoas** | OpenBSD doas 的便携实现 | ISC | 轻量级配置文件语法 |
| **escl** | 极简 doas 风格特权提升程序 | — | 简洁的实现参考 |

### 6.2 macOS 风格参考

| 项目 | 描述 | 许可证 | 参考价值 |
| :--- | :--- | :--- | :--- |
| **OCForks/SMJobBless** | SMJobBless 官方示例实现 | Apple Sample Code | 特权辅助工具安装与验证 |
| **aronskaya/smjobbless** | SMJobBless + XPC 通信示例 | — | 特权工具与主应用 IPC |
| **STPrivilegedTask** | AuthorizationExecuteWithPrivileges 封装 | — | 旧版授权 API 的封装模式 |

### 6.3 Windows UAC 风格参考

| 项目 | 描述 | 许可证 | 参考价值 |
| :--- | :--- | :--- | :--- |
| **UACME** | Windows UAC 绕过方法收集 | — | 完整性级别模型理解 |
| **RunasCs** | Windows UAC 检查与绕过 | — | 权限提升流程参考 |
| **buac** | Rust 实现的 Windows UAC 库 | — | 最小化实现思路 |

### 6.4 通用参考

| 项目 | 描述 | 许可证 | 参考价值 |
| :--- | :--- | :--- | :--- |
| **iamroot** | 用户态特权系统调用模拟 | — | 非特权进程执行特权操作 |
| **SigmaOS** | 混合内核架构 | — | 内核权限模型参考 |


## 七、实施路线图

### Phase 1：内核基础（1-2 周）

- [ ] 在 `task_t` 中添加 `integrity_level` 字段
- [ ] 实现能力位图（`capabilities[2]`）
- [ ] 实现基础权限检查函数 `suki_check_permission`
- [ ] 实现 `suki_getuid`、`suki_geteuid`、`suki_getgid` 系统调用
- [ ] 在 `task_create` 时初始化安全令牌
- [ ] 增加内核启动参数解析（`suki.debug=1`），创建调试角色

### Phase 2：策略引擎与 Shell 授权（2-3 周）

- [ ] 实现策略配置文件解析器（doas 风格）
- [ ] 实现 `suki_check_policy` 函数
- [ ] 实现 `AUTH_SERVER` 系统服务（监听 AUTH_PORT）
- [ ] 实现 Shell 授权提示（英文界面）
- [ ] 实现密码验证与授权令牌签发
- [ ] 实现授权缓存（时间窗口）

### Phase 3：特权辅助工具（1-2 周）

- [ ] 设计特权辅助工具框架
- [ ] 实现 `HELPER_PORT` 通信协议
- [ ] 实现应用 ↔ 辅助工具的 IPC
- [ ] 实现辅助工具的启动与管理

### Phase 4：GUI 集成（窗口管理器就绪后）

- [ ] 将 Shell 授权提示迁移为 GUI 对话框（英文）
- [ ] 实现图形化授权界面（显示服务扩展）
- [ ] 支持 Touch/生物识别（可选）

### Phase 5：审计与完善（1-2 周）

- [ ] 实现审计日志
- [ ] 实现 `suki_audit_read` 系统调用
- [ ] 编写测试套件
- [ ] 加固调试角色的启用条件（仅允许开发/测试环境）


## 八、错误码定义

```c
// include/sukios/security.h

#define SUKI_EACCES         -1   // 权限不足
#define SUKI_EPERM_DENIED   -2   // 权限请求被拒绝
#define SUKI_EPERM_TIMEOUT  -3   // 权限请求超时
#define SUKI_EPASS_INVALID  -4   // 密码错误
#define SUKI_EACCOUNT_LOCKED -5  // 账户已锁定
#define SUKI_ESESSION_EXPIRED -6 // 会话已过期
#define SUKI_ECAP_INVALID   -7   // 无效的能力
#define SUKI_EAUTH_EXPIRED  -8   // 授权令牌已过期
#define SUKI_EAUTH_REVOKED  -9   // 授权令牌已撤销
```


## 九、总结

这套混合风格权限模型的核心设计如下：

| 特性 | 来源 | SukiOS 实现 |
| :--- | :--- | :--- |
| **完整性级别** | Windows UAC | `integrity_level` 字段，含调试级别 5 |
| **按需授权** | macOS Authorization | `suki_auth_token_t` + AUTH_SERVER |
| **特权辅助工具** | macOS SMJobBless | 独立的 `privileged_helper` 服务 |
| **策略配置** | OpenBSD doas / Linux Polkit | `/etc/suki/auth.conf` |
| **Shell 授权** | 定制 | 无 GUI 环境的命令行授权交互（英文） |
| **端口权能传递** | Mach | 授权令牌通过 Mach 端口传递 |
| **能力位图** | Linux Capabilities | 128 位能力空间 |
| **调试角色** | 定制 | `SUKI_IL_DEBUG`，内核参数启用，无条件全权限 |

**关键优势**：
1. 不依赖窗口管理器即可工作（Shell 授权提示）
2. 窗口管理器就绪后可平滑升级到 GUI 对话框
3. 充分利用已有的 Mach 端口权能机制
4. 配置文件驱动，系统管理员可灵活定制策略
5. 借鉴了 Windows、macOS、Linux 三大系统的成熟设计
6. 调试角色为开发和故障诊断提供便利，且不影响生产环境安全模型