# Step 34：IDT 软件可触发异常门改为 DPL=3（#BP/#OF/#UD）

本步是 Step 33（OSDev 基础模块合规检查）中「判定不改」项（IDT #BP/#OF 门 DPL）的
翻转修正。用户要求将可软件触发的异常门改为 DPL=3，本文记录改动、依据与验证。

## 一、背景与依据

### 1.1 问题本质
x86_64 IDT 门的 `type_attr` 字节中，**DPL 字段只对「软件中断指令」（`int n` /
`int3` / `into` / `bound`）的合法性检查生效**；CPU 硬件异常（#PF、#UD 由非法
指令触发等）的投递不受门 DPL 约束（异常总是能进对应的门，无论 CPL）。

但在 SukiOS 原先实现中，`idt_init` 统一把 0..47 向量都设为 `0x8E`
（present, **DPL0**, 中断门）。这意味着：

- 用户态执行 `int3`（断点指令）→ CPU 检查门 #BP(3) 的 DPL=0，而 CPL=3，
  **权限不足 → 门拦截失败 → 转而触发 #GP(13)**，而不是进入 #BP 处理路径。
- 同理 `into`（溢出）、`ud2`（未定义指令，常被用作断言/调试陷阱）在用户态
  也会因门 DPL0 被转为 #GP。

这与 OSDev《IDT》规范的明确建议冲突：**#BP(3) 和 #OF(4) 设计为允许 Ring3
软件中断触发，其门 DPL 必须为 3**。

### 1.2 改动目标
将 `#BP`(3)、`#OF`(4)、`#UD`(6) 三个「软件可触发」异常门由 DPL0（`0x8E`）
改为 DPL3（`0xEE` = present + 中断门 + DPL3），使：
- 用户态 `int3`/`into`/`ud2` 能**合法地**经对应的 #BP/#OF/#UD 门进入内核处理路径；
- 不再因门 DPL0 被误转为 #GP，造成「本应是断点/溢出/非法指令语义」被错误归类为
  通用保护异常。

同时保证**闭环安全**（生产稳定性铁律）：用户态触发这些异常必须有确定性归宿，
不能让一次用户 `int3` 把整个内核拖入 oops/停机。

## 二、实现细节

### 2.1 修改文件
`/mnt/d/Projects/SukiOS/kernel/arch/x86_64/idt.c`

### 2.2 门属性按向量区分（idt_init 内循环）
```c
for (int i = 0; i < 48; i++) {
    uint8_t ist = (i == 8) ? 1 : ((i == 2) ? 2 : 0);
    /* 软件可触发异常：DPL3（用户态指令可门进入），其余 DPL0。 */
    uint8_t ta = (i == 3 || i == 4 || i == 6) ? 0xEE : 0x8E;
    idt_set_gate(i, (uint64_t)g_stubs[i], ist, ta);
}
```
- `0xEE` = bit7(present) + type=0b1110(中断门，自动关中断) + DPL=0b11。
- 仅 3/4/6 三个软件可触发异常用 DPL3；其余异常（#PF、#GP、#DF 等）保持 DPL0，
  硬件异常投递不受 DPL 影响，行为不变。
- 双故障(8)=IST1、NMI(2)=IST2 的 IST 分配保持不变。

### 2.3 注册闭环 handler（sw_breakpoint_handler）
```c
static void sw_breakpoint_handler(registers_t *r)
{
    bool user = (r->cs & 0x3) != 0;
    int  vec  = (int)r->int_no;
    const char *name = (vec < 32) ? g_exc_names[vec] : "?";

    if (user) {
        task_t *t = sched_current();
        kprintf("[exc] user %s(#%d): pid=%lu '%s' rip=%p -> killing task\n",
                name, vec, (unsigned long)t->id, t->name, (void *)r->rip);
        task_exit_current(139);          /* noreturn：隔离故障任务（139≈SIGSEGV） */
    }
    kprintf("\n[KEXC] KERNEL %s(#%d) at rip=%p cs=0x%lx rflags=0x%lx\n",
            name, vec, (void *)r->rip,
            (unsigned long)r->cs, (unsigned long)r->rflags);
    kernel_oops("Kernel software-triggered exception", r);
}
```
注册：
```c
register_interrupt_handler(3, sw_breakpoint_handler);
register_interrupt_handler(4, sw_breakpoint_handler);
register_interrupt_handler(6, sw_breakpoint_handler);
```

### 2.4 闭环语义（复用 #PF 用户态隔离模式）
| 来源 | 行为 | 依据 |
|------|------|------|
| 用户态（cs&3≠0）触发 #BP/#OF/#UD | 杀掉当前用户任务（`task_exit_current(139)`），返回调度 | 与 `page_fault_handler` 用户态隔离一致，故障任务被隔离，内核继续运行 |
| 内核态触发 | 打印现场 + `kernel_oops`（保留内核调试栈帧） | 内核 `ud2`/`int3` 视为断言/bug，按既有内核 oops 路径处理 |

### 2.5 汇编框架兼容性
`isr.S` 中 `#BP`(3)、`#OF`(4)、`#UD`(6) 均用 `ISR_NOERR`（CPU 不压错误码，
存根压哑值 0），与 handler 中读取 `r->err_code` 无关，帧结构一致，无需改动。

KPTI 保存/还原逻辑（`isr_common`）对所有向量统一处理，DPL 改动不影响 CR3
影子视图切换语义。

### 2.6 日志增强
`idt_init` 末尾打印：
```
[idt] IDT loaded (48+80(MSI)+IPI vectors, #PF/#BP/#OF/#UD handlers)
      #BP/#OF/#UD gates DPL3 (user int3/into/ud2 allowed)
```

## 三、验证

### 3.1 编译
`make iso` 通过，无新增 lint 错误（read_lints 0 项）。

### 3.2 单核生产回归（bash + QEMU 机制）
命令（符合项目规则，仅用 QEMU 自带机制 + 串口落盘 + `-d int`）：
```
qemu-system-x86_64 -machine pc -cpu qemu64 -smp 1 -m 2G -no-shutdown \
  -display none -serial file:build/boot.log -boot d -cdrom build/SukiOS.iso \
  -d int -D build/boot.int.log
```
结果（TEST_LAYER=0 基础启动）：
- IDT 日志正确显示 DPL3 门生效；
- CRASH SCAN 全空（无 panic / system halted / Triple / KERNEL page fault / DEADLOCK）；
- `-d int` 日志仅含正常中断：`v=20`（LAPIC 100Hz 节拍，13 次）、`v=f0`（重调度 IPI，2 次），
  **无任何 #GP / #PF / #UD 异常向量**；
- 系统稳定到达 `kernel fully stable`。

### 3.3 关于「用户态 int3 确实走 #BP」的判定
DPL3 位组合 `0xEE` 在代码中确定正确（present + 中断门 + DPL=3）。OSDev《IDT》
规范明确要求 #BP/#OF 门 DPL3，handler 已闭环（用户态杀任务 / 内核态 oops）。
用户态服务链路（TEST_LAYER≥3）的既有 hang 不在本次改动文件范围内，本次改动
未触及存储/用户态服务代码；基础启动全模块零 panic，符合交付标准。

## 四、与 Step 33 的关系
Step 33 将 #BP/#OF 门 DPL 列为「判定不改」，理由是担心改 DPL3 后缺乏闭环
handler 会导致用户 `int3` 进入未处理异常 oops。本步补上了专用闭环
`sw_breakpoint_handler`（用户态隔离 + 内核态 oops），消除了该风险，故按用户
要求落实 DPL3 修正。

## 五、关键常量/地址
- 门 DPL0：`0x8E`（present + 中断门 + DPL0）
- 门 DPL3：`0xEE`（present + 中断门 + DPL3）
- 软件可触发异常向量：3(#BP)、4(#OF)、6(#UD)
- IST 分配：#DF(8)=IST1、NMI(2)=IST2（不变）
- 用户态故障隔离退出码：`task_exit_current(139)`（≈ SIGSEGV）
