#ifndef SUKIOS_ELF64_H
#define SUKIOS_ELF64_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* e_ident 索引 */
#define EI_MAG0      0
#define EI_MAG1      1
#define EI_MAG2      2
#define EI_MAG3      3
#define EI_CLASS     4
#define EI_DATA      5
#define EI_VERSION   6
#define EI_OSABI     7
#define EI_ABIVERSION 8

/* ---- ELF64 类型 ---- */
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;

/* ---- ELF Header ---- */
#define EI_NIDENT 16
typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf64_Half    e_type;
    Elf64_Half    e_machine;
    Elf64_Word    e_version;
    Elf64_Addr    e_entry;
    Elf64_Off     e_phoff;
    Elf64_Off     e_shoff;
    Elf64_Word    e_flags;
    Elf64_Half    e_ehsize;
    Elf64_Half    e_phentsize;
    Elf64_Half    e_phnum;
    Elf64_Half    e_shentsize;
    Elf64_Half    e_shnum;
    Elf64_Half    e_shstrndx;
} Elf64_Ehdr;

/* ---- Program Header ---- */
typedef struct {
    Elf64_Word    p_type;
    Elf64_Word    p_flags;
    Elf64_Off     p_offset;
    Elf64_Addr    p_vaddr;
    Elf64_Addr    p_paddr;
    Elf64_Xword   p_filesz;
    Elf64_Xword   p_memsz;
    Elf64_Xword   p_align;
} Elf64_Phdr;

/* ---- Segment types ---- */
#define PT_NULL     0
#define PT_LOAD     1
#define PT_DYNAMIC  2
#define PT_INTERP   3
#define PT_NOTE     4
#define PT_SHLIB    5
#define PT_PHDR     6

/* ---- Segment flags ---- */
#define PF_X  (1 << 0)
#define PF_W  (1 << 1)
#define PF_R  (1 << 2)

/* ---- 识别魔术 ---- */
#define ELFMAG0  0x7F
#define ELFMAG1  'E'
#define ELFMAG2  'L'
#define ELFMAG3  'F'

/* ---- 结果结构体（由加载器返回） ---- */
typedef struct {
    uint64_t      entry_point;   /* 用户态入口虚拟地址 */
    uint64_t      size;          /* 程序镜像所需的最大虚拟地址跨度（base_extent） */
    uint64_t      base_vaddr;    /* 程序中所有 PT_LOAD 的最小 vaddr */
    uint64_t      max_vaddr;     /* 程序中最大 vaddr + memsz */
    bool          success;       /* 是否成功 */
    const char    *error;        /* 错误信息 */
} elf_load_result_t;

/* ---- API ---- */
elf_load_result_t elf64_load(const uint8_t *file_data, size_t file_size);

/* 将加载结果映射到新进程的页表；
   如果不需要共享当前进程页表，可传入 base_cr3；传 0 则使用当前页表。
   成功返回 0，失败返回 -1。 */
// int elf64_map_segments(const elf_load_result_t *res, uint64_t base_cr3);

#endif