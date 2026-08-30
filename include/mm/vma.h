/*
 * include/mm/vma.h
 * -----------------------------------------------------------------------------
 * VMA（虚拟内存区域）体系（P0-5：地址空间对象化 + 按需分页 + COW + mmap 骨架）。
 *
 * 模型（对标 Linux vm_area_struct / XNU vm_map_entry 的极简版）：
 *   - 每个用户任务持有一条按地址升序的 VMA 单链表（task_t.vma_list）；
 *   - VMA 只登记「合法区间 + 权限 + 类型」，不立即分配物理页；
 *   - 用户 #PF 时查 VMA：命中且权限允许 -> 现场分配零页补映射（demand
 *     zero-fill）；PTE 带 COW 位的写故障 -> 走 vmm_cow_break 拷贝断开；
 *     未命中 -> 维持原语义杀任务（故障隔离）。
 *   - ELF 段仍为立即装载（内容页必须拷贝，按需化收益低）；按需区域当前
 *     覆盖：mmap 匿名映射、用户栈自动增长区。文件映射接口预留（P1-4 页
 *     缓存就绪后接入）。
 *
 * 锁：全局一把 g_vma_lock（irqsave）。调度域现阶段仅 BSP，锁保护
 *     「任务流 vs 中断路径」并为多核调度铺底。
 *
 * 调用关系：
 *   idt.c::page_fault_handler / syscall.c::user_access_ok -> vma_populate()
 *   syscall.c::sys_mmap/sys_munmap -> vma_insert()/vma_unmap_range()
 *   sched.c::task_create_user_args -> vma_insert(栈增长区)
 *   sched.c::task_exit_current / syscall.c::sys_execve -> vma_destroy_all()
 */
#ifndef _SUKI_MM_VMA_H
#define _SUKI_MM_VMA_H

#include <kernel/types.h>

struct task;

/* VMA 类型 */
#define VMA_TYPE_ANON   1   /* mmap 匿名映射（按需零页填充） */
#define VMA_TYPE_STACK  2   /* 用户栈自动增长区（按需零页填充） */
/* VMA_TYPE_FILE 预留：P1-4 页缓存 + 文件映射 */

typedef struct vm_area {
    uint64_t start;              /* 区间起点（页对齐，含） */
    uint64_t end;                /* 区间终点（页对齐，不含） */
    uint64_t prot;               /* 补页时附加的 PTE 位：PTE_WRITE|PTE_NX 组合
                                  * （PTE_PRESENT|PTE_USER 隐含） */
    uint32_t type;               /* VMA_TYPE_* */
    struct vm_area *next;        /* 按 start 升序单链表 */
} vm_area_t;

/* 登记一个 VMA 区间 [start, end)。与既有 VMA 重叠或参数非法返回 false。 */
bool vma_insert(struct task *t, uint64_t start, uint64_t end,
                uint64_t prot, uint32_t type);

/* 查找覆盖 addr 的 VMA；无则 NULL。 */
vm_area_t *vma_find(struct task *t, uint64_t addr);

/* 缺页救援总入口（#PF 处理器与 copy_*_user 预校验共用）：
 *   - PTE 已存在且带 COW 位的写故障 -> COW 拷贝断开；
 *   - PTE 不存在但落在 VMA 内且权限允许 -> 分配零页补映射；
 *   - 其它情况返回 false（调用方按原语义处理：杀任务/EFAULT）。 */
bool vma_populate(struct task *t, uint64_t addr, bool write);

/* 在 [base, top) 中找一段能放下 len 字节（页对齐）的空闲地址；无则返回 0。
 * sys_mmap 用它在 VMA_MMAP_BASE..TOP 里选址（首次适配，链表升序单趟）。 */
uint64_t vma_find_free(struct task *t, uint64_t base, uint64_t top,
                       uint64_t len);

/* 解除 [start, end) 的映射并回收 VMA 登记（munmap 语义）。
 * 已填充的物理页释放（共享页走引用计数守卫）；支持拆分部分重叠的 VMA。 */
bool vma_unmap_range(struct task *t, uint64_t start, uint64_t end);

/* 释放任务的全部 VMA 节点（物理页由 vmm_destroy_address_space 统一回收，
 * 此处只回收链表元数据）。exit/execve 路径调用。 */
void vma_destroy_all(struct task *t);

/* fork 用：深拷贝父任务的 VMA 链表到子任务（保持升序；任一节点分配失败即
 * 完整回滚并返回 false，绝不留下半截链表）。与 vmm_fork_cow 配对使用——
 * 前者复制「页表」，后者复制「区间登记」，缺一则子进程的按需补页失效。 */
bool vma_clone_all(struct task *dst, const struct task *src);

/* mprotect 语义：把 [start, end) 的权限改为 prot（PTE_WRITE|PTE_NX 组合）。
 * 同时更新 VMA 登记（按需拆分两端，绝不误改相邻区域）与已映射页的 PTE，
 * 并全量刷 TLB。区间未被 VMA 完全覆盖、参数未页对齐或分配失败返回 false
 * （失败时链表保持原状）。 */
bool vma_protect(struct task *t, uint64_t start, uint64_t end, uint64_t prot);

/* mmap 匿名区基址与上限（避开 OOL 窗口 0x600000000000 及用户镜像/栈） */
#define VMA_MMAP_BASE   0x0000500000000000UL
#define VMA_MMAP_TOP    0x0000580000000000UL
#define VMA_MMAP_MAX    (64UL * 1024 * 1024)   /* 单次 mmap 上限 64MB */

/* 用户栈自动增长区大小（栈底之下再延伸 128 页 = 512KB 按需区） */
#define VMA_STACK_GROW_PAGES  128

/* 启动自检：VMA 链表操作 + demand 填充 + COW fork/断开全链路（kmain 调用） */
void vma_selftest(void);

#endif /* _SUKI_MM_VMA_H */
