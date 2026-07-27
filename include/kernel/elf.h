/*
 * include/kernel/elf.h
 * -----------------------------------------------------------------------------
 * ELF64（Executable and Linkable Format, 64-bit）加载器接口与常量定义。
 *
 * 本加载器实现“完整功能、不简化”：
 *   - 支持 ET_EXEC（固定加载基址）与 ET_DYN（PIE，随机基址 + ASLR）；
 *   - 处理全部 PT_LOAD 段：按页加载，清零 BSS（memsz > filesz），按 W^X
 *     设置页权限（代码段 RX 不可写、数据段 RW+NX）；
 *   - 构造符合 System V AMD64 ABI 的初始用户栈：argc / argv[] / envp[] /
 *     auxv（AT_PHDR, AT_PHENT, AT_PHNUM, AT_PAGESZ, AT_ENTRY, AT_BASE,
 *     AT_RANDOM, AT_NULL）；
 *   - 拒绝 PT_INTERP（本系统无用户态动态链接器 ld.so），保持静态加载语义。
 *
 * 调用关系：sched.c::task_create_user 与 syscall.c::sys_execve 均调用
 *           elf_validate() / elf_load()。
 */
#ifndef _SUKI_KERNEL_ELF_H
#define _SUKI_KERNEL_ELF_H

#include <kernel/types.h>

/* ---- e_ident[EI_CLASS / EI_DATA] ---- */
#define ELF_CLASS_64   2
#define ELF_DATA_LE    1

/*
 * 初始栈 argv[]/envp[] 元素个数硬上限（H1 修复）。
 * elf_build_stack() 内部用该值定容局部 VA 数组并在入口强制校验
 * argc/envc <= ELF_ARG_MAX；syscall.c 的 EXEC_ARG_MAX 直接引用本常量，
 * 保证"拷贝上限"与"栈构造容量"永远一致，杜绝两处硬编码漂移导致的
 * 数组越界（曾列为审计 H1 项）。
 */
#define ELF_ARG_MAX    64

/*
 * 初始栈 auxv[] 条目数硬上限（H2 修复）。
 * elf_build_stack() 局部 auxv[] 数组按此值定容，且每追加一条都先校验
 * na < ELF_AUXV_MAX；一旦越限立即 kfree 镜像并返回 0 由调用方回滚。
 * 防止未来扩展 auxv 类型时写越界（曾列为审计 H2 项）。当前实际写入 8 条
 * （含 AT_NULL 收尾），留足余量到 16。
 */
#define ELF_AUXV_MAX   16

/* ---- e_type ---- */
#define ET_NONE   0
#define ET_REL    1
#define ET_EXEC   2
#define ET_DYN    3

/* ---- e_machine ---- */
#define EM_X86_64  62

/* ---- p_type ---- */
#define PT_NULL      0
#define PT_LOAD      1
#define PT_DYNAMIC   2
#define PT_INTERP    3
#define PT_NOTE      4
#define PT_PHDR      6
#define PT_GNU_STACK 0x6474E551

/* ---- p_flags（段权限位，与 ABI 一致）---- */
#define PF_X   (1U << 0)
#define PF_W   (1U << 1)
#define PF_R   (1U << 2)

/* ---- 辅助向量 (auxiliary vector) 类型 ---- */
#define AT_NULL    0
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_PAGESZ  6
#define AT_BASE    7
#define AT_ENTRY   9
#define AT_RANDOM  25

/* ELF64 文件头（packed：字段无对齐填充） */
typedef struct elf64_hdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) elf64_hdr_t;

/* ELF64 程序头（packed） */
typedef struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) elf64_phdr_t;

/* ELF 加载结果 */
typedef struct {
    uint64_t entry;        /* 程序入口（用户虚拟地址） */
    uint64_t stack_top;    /* 初始用户栈指针（指向 argc，16 字节对齐） */
} elf_load_result_t;

/*
 * elf_validate：仅校验 ELF64 头部与程序头表的合法性与边界，
 * 不分配任何资源。返回 true 表示可被 elf_load 安全加载。
 */
bool elf_validate(const void *elf, size_t size);

/*
 * elf_load：把 ELF 映像加载进地址空间 as。
 *   - 处理每个 PT_LOAD 段（分配物理页、拷贝文件内容、清零 BSS、按 W^X 映射）；
 *   - 分配 stack_pages 页用户栈（RW+NX），构造初始栈（argc/argv/envp/auxv）；
 *   - 成功返回 true，out->entry / out->stack_top 有效；失败时返回 false，
 *     且不会在 as 中留下半完成的、可执行的映射（调用方应在失败时
 *     vmm_destroy_address_space(as) 整体回收）。
 * 参数：
 *   as         目标地址空间 PML4 物理地址
 *   elf        内核可访问的 ELF 字节缓冲
 *   size       ELF 缓冲长度
 *   argc       argv 数组元素个数（可为 0）
 *   argv       内核侧字符串指针数组（每个指向 NUL 结尾字符串），可为 NULL
 *   envc       环境变量个数（可为 0）
 *   envp       内核侧环境变量字符串数组，可为 NULL
 *   stack_top  用户栈顶（必须页对齐的高端地址），栈占 [stack_top-pages*PAGE, stack_top)
 *   stack_pages 用户栈页数（>= 4 推荐）
 *   out        输出结果
 */
bool elf_load(uint64_t as, const void *elf, size_t size,
              int argc, const char *const argv[],
              int envc, const char *const envp[],
              uint64_t stack_top, uint64_t stack_pages,
              elf_load_result_t *out);

#endif /* _SUKI_KERNEL_ELF_H */
