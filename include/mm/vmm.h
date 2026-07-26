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

#endif /* _SUKI_MM_VMM_H */
