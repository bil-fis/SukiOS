/*
 * kernel/elf/elf.c
 * -----------------------------------------------------------------------------
 * ELF64 加载器核心实现（手册：废除平坦二进制，改用 ELF 加载器 + execve）。
 *
 * 设计要点：
 *   - elf_validate() 仅做合法性/边界校验，不分配资源；
 *   - elf_load() 把 PT_LOAD 段逐页加载进目标地址空间，清零 BSS，按 W^X 设
 *     权限；分配用户栈并构造符合 System V AMD64 ABI 的初始栈
 *     （argc/argv/envp/auxv）；
 *   - 支持 ET_EXEC（固定基址）与 ET_DYN（PIE，随机基址 + ASLR）；
 *   - 拒绝 PT_INTERP（无动态链接器）。
 *
 * 调用关系：sched.c::task_create_user（首次装载）与
 *           syscall.c::sys_execve（替换当前进程映像）均调用本文件。
 */
#include <kernel/elf.h>
#include <kernel/types.h>
#include <kernel/string.h>
#include <kernel/syscall.h>
#include <kernel/console.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/kmalloc.h>

/* 把内核缓冲写入目标地址空间的某段用户虚拟内存（跨页安全）。
 * 依赖内核高半区已映射全部物理内存，故可用 PHYS_TO_VIRT 访问用户页。 */
static void elf_write_user(uint64_t as, uint64_t va, const void *src, size_t n)
{
    const uint8_t *s = (const uint8_t *)src;
    while (n) {
        uint64_t page_off = va & (PAGE_SIZE - 1);
        uint64_t phys = vmm_translate(as, va);
        if (!phys) {
            return;                 /* 不应发生：调用方已建立映射 */
        }
        uint8_t *kva = (uint8_t *)PHYS_TO_VIRT(phys);
        size_t chunk = PAGE_SIZE - page_off;
        if (chunk > n) {
            chunk = n;
        }
        memcpy(kva + page_off, s, chunk);
        s += chunk;
        va += chunk;
        n  -= chunk;
    }
}

/* 轻量熵源（PIE 随机基址 / AT_RANDOM）：TSC + SplitMix64 风格散列 */
static uint64_t elf_random_base(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t x = ((uint64_t)hi << 32) | lo;
    x ^= x >> 33; x *= 0xFF51AFD7ED558CCDUL; x ^= x >> 33;
    /* 0 .. ~16MiB 的页对齐随机偏移 */
    return (x & 0xFFFUL) << 12;
}

bool elf_validate(const void *elf, size_t size)
{
    const elf64_hdr_t *h = (const elf64_hdr_t *)elf;
    if (size < sizeof(*h)) {
        return false;
    }
    if (h->e_ident[0] != 0x7F || h->e_ident[1] != 'E' ||
        h->e_ident[2] != 'L' || h->e_ident[3] != 'F') {
        return false;
    }
    if (h->e_ident[4] != ELF_CLASS_64) {
        return false;
    }
    if (h->e_ident[5] != ELF_DATA_LE) {
        return false;
    }
    if (h->e_type != ET_EXEC && h->e_type != ET_DYN) {
        return false;
    }
    if (h->e_machine != EM_X86_64) {
        return false;
    }
    if (h->e_phoff == 0 || h->e_phnum == 0) {
        return false;
    }
    if (h->e_phnum > ELF_PHDR_MAX) {     /* M9：程序头表条目数硬上限 */
        return false;
    }
    if (h->e_phentsize < sizeof(elf64_phdr_t)) {
        return false;
    }
    /* M9 修复：e_phoff + e_phnum*phentsize 加法回绕校验（防恶意 ELF 让下标
     * 计算回绕后通过长度检查，继而越界读程序头表）。 */
    uint64_t phtab = (uint64_t)h->e_phoff
                   + (uint64_t)h->e_phnum * h->e_phentsize;
    if (phtab < h->e_phoff || phtab > size) {
        return false;
    }

    const elf64_phdr_t *ph =
        (const elf64_phdr_t *)((const uint8_t *)elf + h->e_phoff);
    for (uint16_t i = 0; i < h->e_phnum; i++) {
        if (ph[i].p_type == PT_INTERP) {
            return false;            /* 本系统无动态链接器 */
        }
        if (ph[i].p_type == PT_LOAD) {
            /* M9 修复：文件偏移与虚拟地址加法回绕校验，杜绝恶意字段令下标
             * 回绕后通过上限检查、把段映射到内核区或越界读内存。 */
            if (ph[i].p_offset + ph[i].p_filesz < ph[i].p_offset) {
                return false;
            }
            if (ph[i].p_offset + ph[i].p_filesz > size) {
                return false;
            }
            if (ph[i].p_vaddr + ph[i].p_memsz < ph[i].p_vaddr) {
                return false;
            }
            uint64_t vend = ph[i].p_vaddr + ph[i].p_memsz;
            if (vend > USER_SPACE_TOP) {
                return false;
            }
        }
    }
    return true;
}

/* 构造初始用户栈（返回初始 %rsp，指向 argc，16 字节对齐）。失败返回 0。 */
static uint64_t elf_build_stack(uint64_t as, uint64_t stack_top,
                                uint64_t stack_pages,
                                int argc, const char *const argv[],
                                int envc, const char *const envp[],
                                uint64_t phdr_va, uint32_t phnum, uint32_t phent,
                                uint64_t entry, uint64_t base)
{
    /* ---- H1 修复：入口参数硬校验 ----
     * 本函数曾依赖"调用方恰好也用 64 上限"的隐式约定为局部 VA 数组定容；
     * 一旦任一调用方（sched.c / syscall.c / 未来新增）传入 argc/envc > 64，
     * 下方 arg_va[i]/env_va[i] 即写越内核栈。现在改为：
     *   1) 容量统一取自共享常量 ELF_ARG_MAX（include/kernel/elf.h）；
     *   2) 入口显式拒绝 argc/envc 越限或为负、以及 argc>0 但 argv==NULL
     *      之类的矛盾组合，失败返回 0 由调用方回滚。 */
    if (argc < 0 || argc > ELF_ARG_MAX || envc < 0 || envc > ELF_ARG_MAX) {
        return 0;
    }
    if ((argc > 0 && !argv) || (envc > 0 && !envp)) {
        return 0;
    }

    size_t cap = (size_t)stack_pages * PAGE_SIZE;
    uint8_t *img = (uint8_t *)kmalloc(cap);
    if (!img) {
        return 0;
    }
    memset(img, 0, cap);   /* 栈镜像必须清零：任何空隙不得含内核堆残留数据 */

    uint64_t sp = cap;               /* 栈顶在 img 中的偏移 */
    uint64_t env_va[ELF_ARG_MAX], arg_va[ELF_ARG_MAX];

    /* 字符串区写入的下界哨兵：sp 低于此值即栈容量不足。
     * 若不设防，超长字符串会令 uint64_t sp 减法回绕成巨大偏移，
     * memcpy(img + sp, ...) 直接越界写内核堆（与 H1 同源的边界缺陷）。 */
    const uint64_t sp_floor = 64;    /* 预留底部保护带 */

    /* 1) 环境变量字符串（从顶向下，记录 VA） */
    for (int i = envc - 1; i >= 0; i--) {
        size_t l = strlen(envp[i]) + 1;
        if (sp < sp_floor + l) {
            kfree(img);
            return 0;                /* 栈空间不足，拒绝加载 */
        }
        sp -= l;
        memcpy(img + sp, envp[i], l);
        env_va[i] = stack_top - cap + sp;
    }
    /* 2) argv 字符串 */
    for (int i = argc - 1; i >= 0; i--) {
        size_t l = strlen(argv[i]) + 1;
        if (sp < sp_floor + l) {
            kfree(img);
            return 0;                /* 栈空间不足，拒绝加载 */
        }
        sp -= l;
        memcpy(img + sp, argv[i], l);
        arg_va[i] = stack_top - cap + sp;
    }
    /* 3) AT_RANDOM：16 字节随机数据（放在 auxv 之上） */
    uint8_t rnd[16];
    for (int i = 0; i < 16; i++) {
        rnd[i] = (uint8_t)(elf_random_base() >> (i & 7));
    }
    if (sp < sp_floor + 16) {
        kfree(img);
        return 0;
    }
    sp -= 16;
    memcpy(img + sp, rnd, 16);
    uint64_t rand_va = stack_top - cap + sp;

    /* 4) auxv 数组（AT_NULL 收尾）。H2 修复：auxv[] 按 ELF_AUXV_MAX 定容，
     * 每条追加前先校验 na < ELF_AUXV_MAX，越限则释放镜像返回 0，彻底杜绝
     * "硬编码 16 容量 + na 无上界" 的越界写栈缺陷（含 AT_NULL 在内共填
     * 8 条，上限 16 留余量）。 */
    struct { uint64_t a; uint64_t b; } auxv[ELF_AUXV_MAX];
    int na = 0;
    if (na >= ELF_AUXV_MAX) { kfree(img); return 0; }
    auxv[na].a = AT_PHDR;   auxv[na++].b = phdr_va;
    if (na >= ELF_AUXV_MAX) { kfree(img); return 0; }
    auxv[na].a = AT_PHENT;  auxv[na++].b = phent;
    if (na >= ELF_AUXV_MAX) { kfree(img); return 0; }
    auxv[na].a = AT_PHNUM;  auxv[na++].b = phnum;
    if (na >= ELF_AUXV_MAX) { kfree(img); return 0; }
    auxv[na].a = AT_PAGESZ; auxv[na++].b = PAGE_SIZE;
    if (na >= ELF_AUXV_MAX) { kfree(img); return 0; }
    auxv[na].a = AT_ENTRY;  auxv[na++].b = entry;
    if (na >= ELF_AUXV_MAX) { kfree(img); return 0; }
    auxv[na].a = AT_BASE;   auxv[na++].b = (base != 0) ? base : 0;  /* PIE 基址 */
    if (na >= ELF_AUXV_MAX) { kfree(img); return 0; }
    auxv[na].a = AT_RANDOM; auxv[na++].b = rand_va;
    if (na >= ELF_AUXV_MAX) { kfree(img); return 0; }
    auxv[na].a = AT_NULL;   auxv[na++].b = 0;

    /* 5) 关键：argc/argv/envp/auxv 必须“连续无空隙”（System V ABI：
     * argv = rsp+8，envp 紧随 argv 的 NULL 之后）。因此先算出整个指针块
     * 大小，预先把最终 rsp 对齐到 16，再自底向上连续写入四部分——绝不能
     * 在写完 argv 后再对齐 argc（那会在 argc 与 argv[] 之间留下空洞，
     * crt0 的 argv=rsp+8 会读到垃圾指针 → 用户态野指针缺页）。 */
    uint64_t block = (uint64_t)na * 16                 /* auxv */
                   + (uint64_t)(envc + 1) * 8          /* envp[] + NULL */
                   + (uint64_t)(argc + 1) * 8          /* argv[] + NULL */
                   + 8;                                /* argc */
    if (block + 64 > sp) {
        kfree(img);
        return 0;                     /* 栈空间不足 */
    }
    uint64_t sp_final = (sp - block) & ~15ULL;         /* rsp%16==0（入口时） */
    uint64_t off = sp_final;

    /* argc */
    uint64_t ac = (uint64_t)argc;
    memcpy(img + off, &ac, 8);
    off += 8;
    /* argv[] + NULL */
    for (int i = 0; i <= argc; i++) {
        uint64_t v = (i < argc) ? arg_va[i] : 0;
        memcpy(img + off, &v, 8);
        off += 8;
    }
    /* envp[] + NULL */
    for (int i = 0; i <= envc; i++) {
        uint64_t v = (i < envc) ? env_va[i] : 0;
        memcpy(img + off, &v, 8);
        off += 8;
    }
    /* auxv */
    for (int i = 0; i < na; i++) {
        memcpy(img + off, &auxv[i], 16);
        off += 16;
    }
    uint64_t rsp_out = stack_top - cap + sp_final;

    elf_write_user(as, stack_top - cap, img, cap);
    kfree(img);
    return rsp_out;
}

bool elf_load(uint64_t as, const void *elf, size_t size,
              int argc, const char *const argv[],
              int envc, const char *const envp[],
              uint64_t stack_top, uint64_t stack_pages,
              elf_load_result_t *out)
{
    const elf64_hdr_t *h = (const elf64_hdr_t *)elf;
    if (!elf_validate(elf, size)) {
        return false;
    }

    uint64_t base = (h->e_type == ET_DYN) ? elf_random_base() : 0;
    const elf64_phdr_t *ph =
        (const elf64_phdr_t *)((const uint8_t *)elf + h->e_phoff);

    /* ---- 加载 PT_LOAD 段 ---- */
    for (uint16_t i = 0; i < h->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) {
            continue;
        }
        uint64_t vaddr      = base + ph[i].p_vaddr;
        uint64_t mem_start  = vaddr & ~((uint64_t)PAGE_SIZE - 1);
        uint64_t mem_end    = (vaddr + ph[i].p_memsz + PAGE_SIZE - 1)
                              & ~((uint64_t)PAGE_SIZE - 1);
        uint64_t seg_fstart = vaddr;                 /* 段内存起点（文件起点） */
        uint64_t seg_fend   = vaddr + ph[i].p_filesz;

        uint64_t flags;
        if (ph[i].p_flags & PF_X) {
            flags = PTE_PRESENT | PTE_USER;                  /* RX，不可写 */
        } else if (ph[i].p_flags & PF_W) {
            flags = PTE_PRESENT | PTE_WRITE | PTE_USER | PTE_NX;  /* RW+NX */
        } else {
            flags = PTE_PRESENT | PTE_USER | PTE_NX;         /* 只读+NX */
        }

        for (uint64_t va = mem_start; va < mem_end; va += PAGE_SIZE) {
            void *phys = pmm_alloc_page();
            if (!phys) {
                return false;        /* 调用方负责 vmm_destroy_address_space(as) */
            }
            uint8_t *kva = (uint8_t *)PHYS_TO_VIRT(phys);

            /* 本页在文件中的有效区间：[seg_fstart, seg_fend) ∩ [va, va+PAGE) */
            uint64_t copy_start = va;
            uint64_t copy_end   = va + PAGE_SIZE;
            if (copy_end > seg_fend) {
                copy_end = seg_fend;
            }
            if (copy_start < seg_fstart) {
                copy_start = seg_fstart;
            }
            if (copy_end > copy_start) {
                uint64_t foff = ph[i].p_offset + (copy_start - seg_fstart);
                uint64_t clen = copy_end - copy_start;
                memcpy(kva + (copy_start - va),
                       (const uint8_t *)elf + foff, clen);
            }
            /* 文件未覆盖部分（页前空隙与 BSS）清零 */
            if (copy_start - va > 0) {
                memset(kva, 0, copy_start - va);
            }
            if (copy_end - va < PAGE_SIZE) {
                memset(kva + (copy_end - va), 0, PAGE_SIZE - (copy_end - va));
            }

            if (!vmm_map_page(as, va, (uint64_t)phys, flags)) {
                pmm_free_page(phys);
                return false;
            }
        }
    }

    uint64_t phdr_va = base + h->e_phoff;
    uint64_t entry   = base + h->e_entry;

    /* ---- 分配用户栈（RW+NX） ---- */
    uint64_t stack_base = stack_top - stack_pages * PAGE_SIZE;
    for (uint64_t i = 0; i < stack_pages; i++) {
        void *phys = pmm_alloc_page();
        if (!phys) {
            return false;
        }
        memset(PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
        if (!vmm_map_page(as, stack_base + i * PAGE_SIZE, (uint64_t)phys,
                          PTE_PRESENT | PTE_WRITE | PTE_USER | PTE_NX)) {
            pmm_free_page(phys);
            return false;
        }
    }

    /* ---- 构造初始栈 ---- */
    uint64_t rsp = elf_build_stack(as, stack_top, stack_pages,
                                   argc, argv, envc, envp,
                                   phdr_va, h->e_phnum, h->e_phentsize,
                                   entry, base);
    if (rsp == 0) {
        return false;
    }

    out->entry     = entry;
    out->stack_top = rsp;
    return true;
}
