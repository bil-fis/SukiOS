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
/* 程序头表条目数硬上限（M9 修复）：恶意 ELF 可设极大 e_phnum 使下标计算
 * 回绕越界读；此处定容并强制校验，与 ELF_ARG_MAX/ELF_AUXV_MAX 同理。 */
#define ELF_PHDR_MAX   64

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
    uint64_t base;         /* 主程序加载基址（ET_DYN 为随机基址，ET_EXEC 为 0） */
} elf_load_result_t;

/* ===================== 动态链接（共享库 / dlopen） ===================== */

/* ---- .dynamic 条目 d_tag ---- */
#define DT_NULL       0
#define DT_NEEDED     1
#define DT_HASH       4
#define DT_STRTAB     5
#define DT_SYMTAB     6
#define DT_RELA       7
#define DT_RELASZ     8
#define DT_RELAENT    9
#define DT_SYMENT     11
#define DT_GNU_HASH   0x6ffffef5
#define DT_RELACOUNT  0x6ffffff9
#define DT_FLAGS      30
#define DT_FLAGS_1    0x6ffffffb
#define DT_PLTREL     2      /* d_val = DT_REL 或 DT_RELA（PLT 重定位类型）*/
#define DT_JMPREL     23     /* .rela.plt / .rel.plt 入口 */
#define DT_PLTRELSZ   33     /* PLT 重定位表字节大小 */

/* ---- 符号绑定 / 类型（st_info 高 4 位=bind，低 4 位=type）---- */
#define STB_LOCAL   0
#define STB_GLOBAL  1
#define STB_WEAK    2
#define STT_NOTYPE  0
#define STT_OBJECT  1
#define STT_FUNC    2
#define ELF_ST_BIND(i)  ((i) >> 4)
#define ELF_ST_TYPE(i)  ((i) & 0xf)

/* ---- 重定位类型（x86-64）---- */
#define R_X86_64_NONE        0
#define R_X86_64_64          1
#define R_X86_64_PC32        2
#define R_X86_64_COPY        5
#define R_X86_64_GLOB_DAT    6
#define R_X86_64_JUMP_SLOT   7
#define R_X86_64_RELATIVE    8
#define ELF64_R_SYM(i)       ((uint32_t)((i) >> 32))
#define ELF64_R_TYPE(i)      ((uint32_t)(i))

/* ---- 动态条目 ---- */
typedef struct elf64_dyn {
    int64_t  d_tag;
    uint64_t d_val;
} __attribute__((packed)) elf64_dyn_t;

/* ---- 重定位条目（RELATIVE 的 r_sym=0）---- */
typedef struct elf64_rela {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} __attribute__((packed)) elf64_rela_t;

/* ---- 符号表条目 ---- */
typedef struct elf64_sym {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} __attribute__((packed)) elf64_sym_t;

#define SHN_UNDEF  0

/* ---- 已加载模块（内核态记录，供符号解析与重定位）----
 * 每个被加载的 ELF（主程序或 .sl 共享库）对应一个 elf_module_t。
 * img 为内核侧 ELF 文件副本（符号表/字符串表/重定位表均在此副本内；ET_DYN
 * 基址为 0，故文件偏移 == 虚拟地址，指针可用 img + 偏移表达）。 */
#define ELF_MODULE_MAX  32
typedef struct elf_module {
    uint64_t      base;          /* 加载基址 */
    uint8_t      *img;           /* 内核侧 ELF 文件副本（进程退出时释放） */
    size_t        imgsize;
    char          name[64];      /* 库名（DT_NEEDED 字符串）或 "main" */
    elf64_dyn_t  *dynamic;       /* .dynamic 数组（DT_NULL 结尾） */
    elf64_sym_t  *symtab;        /* .dynsym */
    const char   *strtab;        /* .dynstr */
    uint32_t      symcount;      /* .dynsym 条目数 */
    elf64_rela_t *rela;         /* .rela.dyn */
    uint32_t      relacount;
    elf64_rela_t *rela_plt;     /* .rela.plt */
    uint32_t      relapltcount;
    int           refcount;      /* dlopen 引用计数（load-time 依赖 +1，dlopen +1）*/
    int           resident;      /* 槽位是否已加载（dlclose 归零后留作空闲槽复用）*/
    int           img_owned;     /* img 是否为 kmalloc 副本（dlopen 加载=1，需释放；
                                    * 主程序的内嵌静态 blob=0，不可 kfree，否则损坏内核堆）*/
} elf_module_t;

/*
 * elf_validate：仅校验 ELF64 头部与程序头表的合法性与边界，
 * 不分配任何资源。返回 true 表示可被 elf_load 安全加载。
 */
bool elf_validate(const void *elf, size_t size);

/*
 * elf_load：把 ELF 映像加载进地址空间 as。
 *   - 处理每个 PT_LOAD 段（分配物理页、拷贝文件内容、清零 BSS、按 W^X 映射）；
 *   - 分配 stack_pages 页用户栈（RW+NX），构造初始栈（argc/argv/envp/auxv）；
 *   - 成功返回 true，out->entry / out->stack_top / out->base 有效；失败时返回
 *     false，且不会在 as 中留下半完成的、可执行的映射。
 */
bool elf_load(uint64_t as, const void *elf, size_t size,
              int argc, const char *const argv[],
              int envc, const char *const envp[],
              uint64_t stack_top, uint64_t stack_pages,
              elf_load_result_t *out);

struct task;   /* 前向声明：避免与 task.h 循环包含 */

/*
 * 动态链接：在 as 中已加载主程序（entry 就绪）的基础上，解析主程序 .dynamic，
 * 递归加载 DT_NEEDED 依赖的共享库（.sl，约定路径 /LIB/<name>），对所有已加载
 * 模块应用重定位。成功返回 true，模块信息登记到 t->modules；失败返回 false。
 * 应在 as 建立后、可能切换 cr3 之前调用（写用户空间重定位才可达）。
 */
bool elf_link_dynamic(struct task *t, uint64_t as,
                      const uint8_t *main_elf, size_t main_size,
                      uint64_t main_base, uint64_t main_entry);

/* 运行时 dlopen：把 path 指向的 .sl 加载进当前进程地址空间并链接，
 * 返回模块句柄（>=1，0 保留给主程序）；失败返回 0。符号跨 t->modules 解析。 */
int elf_dlopen(struct task *t, const char *path);

/* 在模块 h 中按名查找符号，返回其运行时地址（base+st_value），未找到返回 0。 */
uint64_t elf_dlsym(struct task *t, int h, const char *name);

/* dlclose：引用计数减一，归零则真正卸载（解映射用户页、释放物理页与
 * 内核 ELF 副本，槽位标记为空闲可复用）。返回 0 成功，-1 非法句柄。 */
int elf_dlclose(struct task *t, int h);

/* 进程退出时释放全部已加载模块的 ELF 副本缓冲（kmalloc 内存），避免
 * 内核堆泄漏。地址空间物理页由 vmm_destroy_address_space 负责回收。 */
void elf_free_modules(struct task *t);

#endif /* _SUKI_KERNEL_ELF_H */
