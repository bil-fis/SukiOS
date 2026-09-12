/*
 * include/kernel/ksym.h
 * -----------------------------------------------------------------------------
 * 内核符号导出表（ksymtab）：供 .kdr 内核模块在加载时解析其引用的内核 API。
 *
 * 设计：
 *   - EXPORT_SYMBOL(sym) 把一个 {名字, 地址} 条目放入专用链接节 .ksymtab；
 *   - 链接脚本（boot/linker.ld）把 .ksymtab 收集为 __ksymtab_start/__ksymtab_end；
 *   - kdr 加载器对模块中未定义的外部符号调用 ksym_lookup() 按名字取地址，并
 *     应用 R_X86_64_64 / GLOB_DAT / JUMP_SLOT 重定位。
 *
 * 这是「内核模块」能在不静态链接到内核的前提下复用 kprintf/kmalloc/driver_register
 * 等内核设施的基础（等价于 Linux 的 EXPORT_SYMBOL + kallsyms 最小子集）。
 */
#ifndef _SUKI_KERNEL_KSYM_H
#define _SUKI_KERNEL_KSYM_H

#include <kernel/types.h>

typedef struct ksym_entry {
    const char *name;     /* 符号名（字符串常量） */
    uint64_t    value;    /* 内核虚拟地址（0 = 未定义） */
} ksym_entry_t;

/* 链接脚本注入的节边界符号（boot/linker.ld 定义） */
extern ksym_entry_t __ksymtab_start[];
extern ksym_entry_t __ksymtab_end[];

/*
 * 注册一个内核符号供模块重定位解析。
 * 注意：仅可导出非 static 的全局符号（取 &sym 地址）；模块侧通过 ksym_lookup
 * 以名字找到本条目，得到其内核虚拟地址。
 */
#define EXPORT_SYMBOL(sym)                                                     \
    static const ksym_entry_t __ksym_##sym                                     \
        __attribute__((used, section(".ksymtab"))) = {                         \
            #sym, (uint64_t)(uintptr_t)(&sym) }

/* 按名字查找内核导出符号地址；未找到返回 0。 */
uint64_t ksym_lookup(const char *name);

/* 启动期诊断：打印已导出符号数量（验证 .ksymtab 节被正确收集）。 */
void ksym_dump_count(void);

#endif /* _SUKI_KERNEL_KSYM_H */
