/*
 * kernel/ksym.c
 * -----------------------------------------------------------------------------
 * 内核符号导出表运行时查询。
 *
 * .ksymtab 节由 EXPORT_SYMBOL 在链接期填充，boot/linker.ld 暴露
 * __ksymtab_start / __ksymtab_end。kdr 加载器对模块中未定义的外部符号调用
 * ksym_lookup() 解析其内核虚拟地址，从而让内核模块无需静态链接即可调用
 * kprintf / kmalloc / driver_register 等内核 API。
 */
#include <kernel/ksym.h>
#include <kernel/console.h>
#include <kernel/string.h>

uint64_t ksym_lookup(const char *name)
{
    for (ksym_entry_t *e = __ksymtab_start; e < __ksymtab_end; e++) {
        if (e->name && strcmp(e->name, name) == 0)
            return e->value;
    }
    return 0;
}

void ksym_dump_count(void)
{
    size_t n = (size_t)(__ksymtab_end - __ksymtab_start);
    kprintf("[ksym] exported %u kernel symbols\n", (uint32_t)n);
}
