/*
 * include/mm/vmm.h
 * -----------------------------------------------------------------------------
 * 虚拟内存管理器 (VMM)：4 级分页 (PML4->PDPT->PD->PT)，4KB 页。
 *
 * 页表项标志位。
 */
#ifndef _SUKI_MM_VMM_H
#define _SUKI_MM_VMM_H

#include <kernel/types.h>

#define PTE_PRESENT   (1UL << 0)
#define PTE_WRITE     (1UL << 1)
#define PTE_USER      (1UL << 2)
#define PTE_PWT       (1UL << 3)
#define PTE_PCD       (1UL << 4)
#define PTE_HUGE      (1UL << 7)
#define PTE_NX        (1UL << 63)
#define PTE_OOL       (1UL << 9)   /* 软件位：该 PTE 映射的是 IPC OOL 共享页 */
#define PTE_COW       (1UL << 10)  /* 软件位：写时复制页（只读共享，写故障时
                                    * 由 vmm_cow_break 拷贝断开，P0-5） */

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000UL

void     vmm_init(void);

/* 返回内核 PML4 的物理地址 */
uint64_t vmm_kernel_pml4(void);

/* 在指定地址空间(PML4 物理地址)建立 4KB 映射；按需分配中间页表 */
bool     vmm_map_page(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);
void     vmm_unmap_page(uint64_t pml4_phys, uint64_t virt);

/* 查询 virt 对应的物理地址（未映射返回 0） */
uint64_t vmm_translate(uint64_t pml4_phys, uint64_t virt);

/* 查询 virt 对应的叶级页表项(PTE)原始值（含标志位），未映射返回 0。
 * 供 copy_from_user/copy_to_user 做逐页权限校验（A1 项）。 */
uint64_t vmm_pte(uint64_t pml4_phys, uint64_t virt);

/* 创建新地址空间：分配 PML4 并共享内核高半区映射，返回 PML4 物理地址 */
uint64_t vmm_create_address_space(void);

/* 销毁用户地址空间：释放用户半区(0..255)的所有页表结构与“自有”物理页
 * （OOL 共享页只解除映射不释放，引用计数由 OOL 清理路径处理，B1 项）。 */
void     vmm_destroy_address_space(uint64_t pml4_phys);

/* 把 [0, highest) 全部物理内存映射到内核高半区（替换引导期临时 2MB 大页
 * 仅覆盖 4GB 的限制，支持 >4GB 内存，B4 项）。 */
void     vmm_extend_kernel_mapping(uint64_t highest);

/* 切换当前地址空间（加载 CR3） */
void     vmm_switch(uint64_t pml4_phys);

/* ---- P0-5：写时复制（COW） ----
 * vmm_fork_cow：把 src 用户半区(0..255)的全部已映射页共享给 dst：
 *   - 可写页：双方 PTE 均改为 只读+PTE_COW，物理页引用计数 +1；
 *   - 只读页：直接共享（引用计数 +1，无需 COW 位）；
 *   - PTE_OOL 页跳过（OOL 生命周期由 IPC 引用计数独立管理，fork 语义
 *     不复制 OOL 窗口——与 XNU 对 VM_INHERIT_NONE 区域的处理一致）。
 * 完整 fork() syscall（复制内核栈/寄存器上下文）随 P1-5 落地，本函数是
 * 其地址空间层地基，当前由 vma_selftest 全链路验证。 */
bool     vmm_fork_cow(uint64_t src_pml4, uint64_t dst_pml4);

/* 对带 PTE_COW 位的页执行写时复制断开：
 *   引用计数 >1 -> 分配新页拷贝内容，改映射为可写，旧页 decref；
 *   引用计数==1 -> 最后持有者，直接改回可写并清 COW 位。
 * 返回 false 表示该 PTE 不是 COW 页或资源不足。 */
bool     vmm_cow_break(uint64_t pml4_phys, uint64_t virt);

/* ========================================================================= */
/*  P0-8 / P0-R2：KPTI（KAISER 式双页表，Meltdown 缓解）                       */
/* ========================================================================= */
/*
 * 模型：每个用户地址空间由「物理连续、8KB 对齐的一对 PML4」构成：
 *   偶页（pair+0）  = 内核视图：用户半区 + 完整内核高半区（task->cr3 存它）；
 *   奇页（pair+4K） = 影子视图：用户半区（共享同一批下级页表）+ 最小内核窗口
 *                     （内核映像 .text/.rodata/.data/.bss + IST 守卫栈 +
 *                      本任务内核栈——CPU 从 Ring3 进入时压栈/取指所需的
 *                      全部页；物理直映区/内核堆/他任务栈一律不映射）。
 * CR3 切换只翻转 bit12（成对物理相邻保证）：
 *   Ring3 运行           -> CR3 = 影子（bit12=1）
 *   syscall/中断入内核    -> CR3 &= ~bit12（syscall_entry.S / isr.S）
 *   iretq/sysret 回用户  -> CR3 |= bit12（g_kpti_enabled 门控）
 * 未启用（RDCL_NO=1 硬件免疫）时地址空间退化为单页 PML4，行为与旧版一致。
 */

/* 1 = KPTI 已启用（security_init 依 Meltdown 免疫检测置位，此后不再变化；
 * 必须在任何“会进入 Ring3 的地址空间”创建之前定值）。汇编入口按位读取。 */
extern uint8_t g_kpti_enabled;

/* 把任务内核栈 [kstack_base, kstack_top) 覆盖的页映射进该地址空间的影子
 * PML4（Ring3 被中断时 CPU 直接向 TSS.rsp0 压栈，栈页必须在影子中可写）。
 * KPTI 未启用时为空操作。task_create_user_args / sys_execve 调用。 */
void vmm_kpti_map_kstack(uint64_t pml4_phys, uint64_t kstack_base,
                         uint64_t kstack_top);

#endif /* _SUKI_MM_VMM_H */
