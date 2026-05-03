#include "userland/elf64.h"
#include "kernel/vmm.h"
#include "kernel/heap.h"
#include "kernel/pmm.h"
#include "kernel/tty.h"

/**
 * elf64_load - 直接从内存缓冲区加载 ELF64，解析所有 PT_LOAD 段并映射到用户地址空间
 *
 * 加载步骤严格参照 OSDev ElfLoading 教程（User:Joeeagar/ElfLoading）：
 *  1. 验证魔术 [0x7F 'E' 'L' 'F']
 *  2. 验证 64 位 (e_ident[4]==2) 和 x86-64(e_machine==62)
 *  3. 遍历程序头表，计算基址 (min p_vaddr) 和最高地址
 *  4. 对每个 PT_LOAD 段，从 p_vaddr 开始逐页分配物理页并映射（清空未初始化的部分）
 *  5. 拷贝文件数据到映射的虚拟地址
 *
 * 返回的 elf_load_result_t 可用于进一步初始化 PCB。
 */
elf_load_result_t elf64_load(const uint8_t *file_data, size_t file_size) {
    elf_load_result_t result = {0};
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)file_data;
    const Elf64_Phdr *phdr;
    uint64_t base = UINT64_MAX;
    uint64_t max_addr = 0;
    int i;

    /* ---- 1. 验证 ELF 魔术 ---- */
    if (file_size < sizeof(Elf64_Ehdr) ||
        ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        result.error = "Invalid ELF magic";
        return result;
    }

    /* ---- 2. 验证 64 位 x86-64 ---- */
    if (ehdr->e_ident[EI_CLASS] != 2) {  /* 2 = ELFCLASS64 */
        result.error = "Not a 64-bit ELF";
        return result;
    }
    if (ehdr->e_machine != 62) {         /* 62 = EM_X86_64 */
        result.error = "Not an x86_64 ELF";
        return result;
    }

    /* ---- 3. 遍历程序头表，计算基址和最大地址 ---- */
    if (ehdr->e_phoff + ehdr->e_phentsize * ehdr->e_phnum > file_size) {
        result.error = "Program headers beyond file size";
        return result;
    }
    phdr = (const Elf64_Phdr *)(file_data + ehdr->e_phoff);

    for (i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD) {
            if (phdr[i].p_vaddr < base)
                base = phdr[i].p_vaddr;
            uint64_t end = phdr[i].p_vaddr + phdr[i].p_memsz;
            if (end > max_addr)
                max_addr = end;
        }
    }

    if (base == UINT64_MAX) {
        result.error = "No PT_LOAD segments found";
        return result;
    }

    /* ---- 4. 映射所有 PT_LOAD 段到用户虚拟地址 ---- */
    for (i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *p = &phdr[i];
        if (p->p_type != PT_LOAD) continue;

        uint64_t seg_start = p->p_vaddr & ~(VMM_PAGE_SIZE - 1);
        uint64_t seg_end   = (p->p_vaddr + p->p_memsz + VMM_PAGE_SIZE - 1) & ~(VMM_PAGE_SIZE - 1);

        for (uint64_t va = seg_start; va < seg_end; va += VMM_PAGE_SIZE) {
            /* 如果该页尚未映射，则分配物理页并映射 */
            if (!vmm_is_mapped(va)) {
                /* 分配物理页并映射为用户可读写（实际权限根据 p_flags 调整） */
                uint64_t perm = VMM_PRESENT | VMM_USER;
                if (va >= p->p_vaddr) {
                    /* 段内部地址 */
                    if (p->p_flags & PF_W) perm |= VMM_WRITE;
                    /* 默认可读可执行，PF_X 目前未强制 NX */
                }

                if (vmm_alloc_map(va, perm) != 0) {
                    result.error = "Failed to map page";
                    return result;
                }
            }
        }

        /* ---- 5. 拷贝文件数据 ---- */
        uint64_t va_copy = p->p_vaddr;
        uint64_t bytes_left = p->p_filesz;
        const uint8_t *src = file_data + p->p_offset;
        uint64_t offset = 0;

        while (bytes_left > 0) {
            size_t chunk = (bytes_left > VMM_PAGE_SIZE) ? VMM_PAGE_SIZE : bytes_left;
            size_t page_offset = (va_copy + offset) & (VMM_PAGE_SIZE - 1);
            chunk = (chunk + page_offset > VMM_PAGE_SIZE) ? VMM_PAGE_SIZE - page_offset : chunk;

            /* 直接写入用户虚拟地址（当前在内核态，可访问用户页） */
            volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)(va_copy + offset);
            for (size_t j = 0; j < chunk; j++) {
                dst[j] = src[offset + j];
            }
            offset += chunk;
            bytes_left -= chunk;
        }

        /* 清零 .bss 部分（memsz > filesz） */
        if (p->p_memsz > p->p_filesz) {
            uint64_t bss_start = p->p_vaddr + p->p_filesz;
            uint64_t bss_size  = p->p_memsz - p->p_filesz;
            uint8_t *bss = (uint8_t *)(uintptr_t)bss_start;
            for (uint64_t k = 0; k < bss_size; k++) bss[k] = 0;
        }
    }

    /* 成功 */
    result.entry_point = ehdr->e_entry;
    result.base_vaddr  = base;
    result.max_vaddr   = max_addr;
    result.size        = max_addr - base;
    result.success     = true;
    return result;
}