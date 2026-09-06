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
 * 依赖内核高半区已映射全部物理内存，故可用 PHYS_TO_VIRT 访问用户页。
 * 注意：vmm_translate() 的返回值【已含页内偏移】，因此取到物理地址后直接用
 * PHYS_TO_VIRT 即定位到目标字节，绝不可再加一次 (va & (PAGE_SIZE-1)) 偏移，
 * 否则会写成「页基址 + 2×页内偏移」的错位地址（COPY 重定位失效的根因）。 */
static void elf_write_user(uint64_t as, uint64_t va, const void *src, size_t n)
{
    const uint8_t *s = (const uint8_t *)src;
    while (n) {
        uint64_t phys = vmm_translate(as, va);   /* 已含页内偏移 */
        if (!phys) {
            return;                 /* 不应发生：调用方已建立映射 */
        }
        uint8_t *kva = (uint8_t *)PHYS_TO_VIRT(phys);   /* 已定位到目标字节 */
        size_t chunk = PAGE_SIZE - (va & (PAGE_SIZE - 1));
        if (chunk > n) {
            chunk = n;
        }
        memcpy(kva, s, chunk);
        s += chunk;
        va += chunk;
        n  -= chunk;
    }
}

/* 在地址空间 as 内，把 src 用户虚拟地址处的 n 字节拷贝到 dst 用户虚拟地址处。
 * 用内核高半区映射（PHYS_TO_VIRT(vmm_translate)）读 src，再用 elf_write_user
 * 写 dst，借助弹跳缓冲解耦读写，正确处理跨页与 src/dst 重叠。用于 R_X86_64_COPY。
 * 同样：vmm_translate 已含页内偏移，读取时 PHYS_TO_VIRT(sphys) 即目标字节，
 * 不得再叠加 (src & (PAGE_SIZE-1))。 */
static void elf_copy_user(uint64_t as, uint64_t dst, uint64_t src, size_t n)
{
    uint8_t buf[256];
    while (n) {
        uint64_t sphys = vmm_translate(as, src);   /* 已含页内偏移 */
        if (!sphys) {
            return;                 /* 源页未映射（不应发生）*/
        }
        const uint8_t *sp = (const uint8_t *)PHYS_TO_VIRT(sphys);  /* 目标字节 */
        size_t chunk = PAGE_SIZE - (src & (PAGE_SIZE - 1));
        if (chunk > n) {
            chunk = n;
        }
        if (chunk > sizeof(buf)) {
            chunk = sizeof(buf);
        }
        memcpy(buf, sp, chunk);
        elf_write_user(as, dst, buf, chunk);
        src += chunk;
        dst += chunk;
        n   -= chunk;
    }
}

/* 共享库 / ET_DYN 加载基址（轻量熵源：TSC + SplitMix64 风格散列）。
 *
 * 关键：必须落在【用户空间高位且远低于栈顶】的区域，绝对不能靠近 0（NULL 页）
 * 或与主映像（USER_CODE_BASE=0x400000）、堆（POSIX_HEAP_BASE=0x4000000000）冲突。
 * 旧实现返回 (x & 0xFFF)<<12（0..~16MiB），会把共享库映射到 0 附近的低位地址，
 * 既可能踩 NULL 页，又可能与主程序 0x400000 重叠 → 运行期数据损坏/缺页。
 *
 * 这里选 0x600000000000（6TiB）为基地，叠加最多 ~1TiB 的随机偏移，结果位于
 * 6~7TiB 区间：远高于主映像与堆（均 < 256GiB），又远低于栈顶（0x7FFFFFFFFFFF，
 * ~128TiB），且天然 4KiB 页对齐。 */
static uint64_t elf_random_base(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t x = ((uint64_t)hi << 32) | lo;
    x ^= x >> 33; x *= 0xFF51AFD7ED558CCDUL; x ^= x >> 33;
    /* 0x600000000000 + (0 .. 0xFFFFFF)<<16 ≈ 6TiB .. 7TiB，远低于栈顶 */
    uint64_t base = 0x600000000000ULL + ((x & 0xFFFFFFUL) << 16);
    return base;   /* 必为 4KiB 对齐且 != 0 */
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

    /* ---- 构造初始栈（stack_pages==0 时跳过栈分配/构造，用于加载共享库）---- */
    if (stack_pages == 0) {
        out->entry     = entry;
        out->stack_top = 0;
        out->base      = base;
        return true;
    }

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
    out->base      = base;
    return true;
}

/* ========================================================================== */
/*  动态链接（共享库 / dlopen）                                                */
/* ========================================================================== */

#include <kernel/task.h>   /* elf_link_dynamic 等需完整 task_t（modules/cr3） */

/* 在 ELF 副本中定位 PT_DYNAMIC，返回 .dynamic 数组指针；无则返回 NULL。 */
static elf64_dyn_t *elf_find_dynamic(const uint8_t *img, size_t size)
{
    const elf64_hdr_t *h = (const elf64_hdr_t *)img;
    if (h->e_phnum == 0) {
        return NULL;
    }
    const elf64_phdr_t *ph = (const elf64_phdr_t *)(img + h->e_phoff);
    for (uint16_t i = 0; i < h->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) {
            if (ph[i].p_offset + ph[i].p_filesz > size) {
                return NULL;
            }
            return (elf64_dyn_t *)(img + ph[i].p_offset);
        }
    }
    return NULL;
}

/* 取 .dynamic 中 d_tag 对应的 d_val（未找到返回 def）。 */
static uint64_t elf_dyn_get(elf64_dyn_t *dyn, uint64_t tag, uint64_t def)
{
    for (elf64_dyn_t *d = dyn; d->d_tag != DT_NULL; d++) {
        if ((uint64_t)d->d_tag == tag) {
            return d->d_val;
        }
    }
    return def;
}

/* 解析 DT_GNU_HASH 得到 .dynsym 总符号数（现代 ld 默认只产 GNU_HASH，不发
 * 经典 DT_HASH）。返回 0 表示无法解析（调用方回退到文件缓冲边界上限）。 */
static uint32_t elf_gnuhash_symcount(const uint8_t *img, size_t size,
                                     uint64_t gnu_off)
{
    if (gnu_off == 0 || gnu_off + 16 > size) {
        return 0;
    }
    const uint32_t *hdr = (const uint32_t *)(img + gnu_off);
    uint32_t nbuckets   = hdr[0];
    uint32_t symoffset  = hdr[1];
    uint32_t bloom_size = hdr[2];
    uint32_t bloom_shift = hdr[3];
    (void)bloom_shift;

    uint64_t bloom_off   = gnu_off + 16;
    uint64_t buckets_off = bloom_off + (uint64_t)bloom_size * 8;
    if (buckets_off + (uint64_t)nbuckets * 4 > size) {
        return 0;
    }
    const uint32_t *buckets = (const uint32_t *)(img + buckets_off);

    /* 找最大 bucket 起始符号（其链尾即整体最后一个符号） */
    uint32_t maxbucket = 0;
    for (uint32_t b = 0; b < nbuckets; b++) {
        if (buckets[b] > maxbucket) {
            maxbucket = buckets[b];
        }
    }
    if (maxbucket < symoffset) {
        return symoffset;   /* 无导出符号，仅有本地符号 [0, symoffset) */
    }

    uint64_t chain_off = buckets_off + (uint64_t)nbuckets * 4;
    uint64_t chain_len = (size > chain_off) ? (size - chain_off) / 4 : 0;
    if (chain_len == 0 || (maxbucket - symoffset) >= chain_len) {
        return 0;
    }
    const uint32_t *chain = (const uint32_t *)(img + chain_off);

    uint32_t s = maxbucket, guard = 0;
    while (guard++ < chain_len) {
        if (s < symoffset || (s - symoffset) >= chain_len) {
            return 0;
        }
        uint32_t e = chain[s - symoffset];
        if (e & 1) {
            return s + 1;   /* s 是 bucket 链尾符号，总数 = s+1 */
        }
        if (e < symoffset || (e - symoffset) >= chain_len) {
            return 0;
        }
        s = e;
    }
    return 0;
}

/* 安全比较：模块 m 的 .dynstr 中偏移 name_off 处的字符串 == name？
 * 全程限制在 [img, img+imgsize) 内，绝不越界读（防内核缺页 panic）。 */
static bool elf_sym_name_eq(const elf_module_t *m, uint64_t name_off,
                            const char *name)
{
    const uint8_t *p   = (const uint8_t *)m->strtab + name_off;
    const uint8_t *end = m->img + m->imgsize;
    if (p < (const uint8_t *)m->img || p >= end) {
        return false;     /* 偏移越界 */
    }
    size_t rem = (size_t)(end - p);
    size_t nl  = strlen(name);
    if (nl >= rem) {
        return false;     /* 名称区剩余不足，不可能匹配 */
    }
    return memcmp(p, name, nl) == 0 && p[nl] == '\0';
}

/* 把 ELF 虚拟地址换算为文件内偏移。ET_DYN 在文件里以 0 为基址（vaddr==offset），
 * 换算后不变；而 ET_EXEC 的 .dynsym/.dynstr/.rela 等段里的 d_val 是「虚拟地址」，
 * 与文件偏移相差一个段基址差，必须按 PT_LOAD 段换算回文件偏移，才能从内核持有的
 * 文件副本（mod->img）正确读取这些表。否则 ET_EXEC 的 d_val（如 0x50xxxx）会远超
 * 文件尺寸，被边界检查误判为非法。 */
static uint64_t elf_vaddr_to_fileoff(const uint8_t *img, uint64_t size, uint64_t vaddr)
{
    (void)size;   /* 仅作接口一致性保留；换算只依赖程序头，无需文件总长 */
    const elf64_hdr_t *h = (const elf64_hdr_t *)img;
    if (h->e_phoff == 0 || h->e_phnum == 0) {
        return vaddr;
    }
    const elf64_phdr_t *ph = (const elf64_phdr_t *)(img + h->e_phoff);
    for (uint16_t i = 0; i < h->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) {
            continue;
        }
        uint64_t lo = ph[i].p_vaddr;
        uint64_t hi = ph[i].p_vaddr + ph[i].p_filesz;
        if (vaddr >= lo && vaddr < hi) {
            return vaddr - ph[i].p_vaddr + ph[i].p_offset;
        }
    }
    return vaddr;   /* 不在任何段内（异常），交由上层边界检查拦截 */
}

/* 解析某模块的 .dynamic，把符号表/字符串表/重定位表指针（指向 img 内偏移）
 * 填入 mod。base 为实际加载基址，img 由调用方持有（供符号解析）。 */
static bool elf_parse_dynamic(elf_module_t *mod, const uint8_t *img,
                              size_t size, uint64_t base)
{
    memset(mod, 0, sizeof(*mod));
    mod->base     = base;
    mod->img      = (uint8_t *)img;
    mod->imgsize  = size;
    mod->refcount = 1;

    elf64_dyn_t *dyn = elf_find_dynamic(img, size);
    if (!dyn) {
        return true;   /* 纯静态模块，无动态信息 */
    }
    mod->dynamic = dyn;

    /* d_val 是虚拟地址，须换算成文件内偏移才能从 img 正确索引（见
     * elf_vaddr_to_fileoff 说明）。 */
    uint64_t strtab_off = elf_vaddr_to_fileoff(img, size, elf_dyn_get(dyn, DT_STRTAB, 0));
    uint64_t symtab_off = elf_vaddr_to_fileoff(img, size, elf_dyn_get(dyn, DT_SYMTAB, 0));
    uint64_t rela_off   = elf_vaddr_to_fileoff(img, size, elf_dyn_get(dyn, DT_RELA, 0));
    uint64_t relasz     = elf_dyn_get(dyn, DT_RELASZ, 0);
    uint64_t relaent    = elf_dyn_get(dyn, DT_RELAENT, sizeof(elf64_rela_t));

    if (strtab_off > size || symtab_off > size ||
        rela_off > size || rela_off + relasz > size) {
        return false;
    }

    mod->strtab = (const char *)(img + strtab_off);
    mod->symtab = (elf64_sym_t *)(img + symtab_off);
    mod->rela   = (elf64_rela_t *)(img + rela_off);
    mod->relacount = (uint32_t)(relasz / (relaent ? relaent : sizeof(elf64_rela_t)));

    /* PLT 重定位（仅支持 RELA） */
    uint64_t pltrel = elf_dyn_get(dyn, DT_PLTREL, 0);
    if (pltrel == DT_RELA) {
        uint64_t jmprel = elf_vaddr_to_fileoff(img, size, elf_dyn_get(dyn, DT_JMPREL, 0));
        uint64_t pltsz  = elf_dyn_get(dyn, DT_PLTRELSZ, 0);
        if (jmprel > size || jmprel + pltsz > size) {
            return false;
        }
        mod->rela_plt = (elf64_rela_t *)(img + jmprel);
        mod->relapltcount = (uint32_t)(pltsz / (relaent ? relaent : sizeof(elf64_rela_t)));
    }

    /* 符号数：优先 DT_HASH.nchain，其次 DT_GNU_HASH 精确解析，最后以文件缓冲
     * 边界作为上限（防越界读）。三者逐级回退，确保 symcount 既不太小也不越界。 */
    uint64_t hash_off  = elf_vaddr_to_fileoff(img, size, elf_dyn_get(dyn, DT_HASH, 0));
    uint64_t gnu_off   = elf_vaddr_to_fileoff(img, size, elf_dyn_get(dyn, DT_GNU_HASH, 0));
    if (hash_off && hash_off + 8 <= size) {
        const uint32_t *h = (const uint32_t *)(img + hash_off);
        mod->symcount = h[1];   /* nchain == nsyms */
    } else if (gnu_off) {
        uint32_t gc = elf_gnuhash_symcount(img, size, gnu_off);
        if (gc != 0) {
            mod->symcount = gc;
        } else if (symtab_off < size) {
            mod->symcount = (uint32_t)((size - symtab_off) / sizeof(elf64_sym_t));
        } else {
            mod->symcount = 0;
        }
    } else if (symtab_off < size) {
        mod->symcount = (uint32_t)((size - symtab_off) / sizeof(elf64_sym_t));
    } else {
        mod->symcount = 0;
    }
    return true;
}

/* 解映射并释放某已加载模块在用户地址空间 as 内的全部 PT_LOAD 物理页。
 * 段范围从模块自身的 ELF 副本（mod->img）重新解析程序头取得，无需在
 * elf_module_t 中额外缓存段表。按页解除 PTE 并 pmm_decref（与
 * vmm_destroy_address_space 一致，正确处理共享页引用计数）。 */
static void elf_unmap_module(uint64_t as, elf_module_t *mod)
{
    const elf64_hdr_t *h = (const elf64_hdr_t *)mod->img;
    if (!h || h->e_phoff == 0 || h->e_phnum == 0) {
        return;
    }
    const elf64_phdr_t *ph = (const elf64_phdr_t *)(mod->img + h->e_phoff);
    for (uint16_t i = 0; i < h->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) {
            continue;
        }
        uint64_t vstart = mod->base + ph[i].p_vaddr;
        uint64_t vend   = vstart + ph[i].p_memsz;
        uint64_t va     = vstart & ~((uint64_t)PAGE_SIZE - 1);
        uint64_t end    = (vend + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);
        for (; va < end; va += PAGE_SIZE) {
            uint64_t phys = vmm_translate(as, va);
            if (phys) {
                pmm_decref((void *)phys);
            }
            vmm_unmap_page(as, va);
        }
    }
}

/* 进程退出时释放全部已加载模块的 ELF 副本缓冲（kmalloc 内存）。
 * 物理页/页表由 vmm_destroy_address_space 负责回收，此处只回收内核堆，
 * 避免每个 dlopen 的 .sl 副本（几十~上百 KB）随进程生命周期持续泄漏。 */
void elf_free_modules(struct task *t)
{
    for (int i = 0; i < t->nmodules; i++) {
        elf_module_t *m = &t->modules[i];
        /* 仅释放 dlopen/load_all_deps 经 exec_read_file 分配的 kmalloc 副本；
         * 主程序的内嵌静态 blob（user_*.start）不可 kfree，否则损坏内核堆。 */
        if (m->img && m->img_owned) {
            kfree(m->img);
        }
        m->img = NULL;
        m->img_owned = 0;
        m->resident = 0;
    }
    t->nmodules = 0;
}

/* 跨全部已加载模块按名解析符号，返回运行时地址（base+st_value）；未找到返回 0。 */
static uint64_t elf_resolve_symbol(elf_module_t *mods, int n, const char *name)
{
    for (int i = 0; i < n; i++) {
        elf_module_t *m = &mods[i];
        if (!m->symtab || !m->strtab) {
            continue;
        }
        for (uint32_t s = 1; s < m->symcount; s++) {   /* 跳过 null 符号（索引 0）*/
            elf64_sym_t *sym = &m->symtab[s];
            if (sym->st_shndx == SHN_UNDEF) {
                continue;
            }
            if (sym->st_name == 0) {
                if (sym->st_info == 0 && sym->st_value == 0) {
                    break;     /* 到达表尾 null 符号 */
                }
                continue;
            }
            if (elf_sym_name_eq(m, sym->st_name, name)) {
                return m->base + sym->st_value;
            }
        }
    }
    return 0;
}

/* 应用单条重定位（写用户空间）。 */
static bool elf_apply_reloc(uint64_t as, elf_module_t *mod,
                            elf_module_t *mods, int n, elf64_rela_t *r)
{
    uint32_t type   = ELF64_R_TYPE(r->r_info);
    uint32_t symidx = ELF64_R_SYM(r->r_info);
    uint64_t addr   = mod->base + r->r_offset;
    uint64_t S = 0;

    /* R_X86_64_COPY：跨模块数据复制重定位（拷贝重定位）。
     * 重定位项所在的模块（通常是主程序）在自身 .dynsym 中用一个 UNDEF 符号
     * 引用某个共享库里的全局变量；运行时链接器需在其它已加载模块中找到该符号
     * 的「定义」，将其初始值复制到本模块 r_offset 处的自有副本（.bss）。
     * 关键：拷贝大小必须取自「定义方」符号的 st_size——本模块的 UNDEF 引用
     * st_size 为 0，不能直接用；否则会误判大小未知而丢弃该重定位。 */
    if (type == R_X86_64_COPY) {
        if (symidx == 0) {
            return false;   /* COPY 必须绑定一个符号以确定源/大小 */
        }
        elf64_sym_t *sym = &mod->symtab[symidx];
        const char *nm = mod->strtab + sym->st_name;
        uint64_t src = 0;
        size_t   sz  = 0;
        for (int k = 0; k < n; k++) {
            elf_module_t *dm = &mods[k];
            if (dm == mod) {
                continue;   /* COPY 的源必须是「其它模块」（共享库）的定义；
                              * 当前模块自身的同名符号只是目标副本占位符
                              *（st_shndx 非 UNDEF、st_value 指向 .bss 副本），
                              * 若以它作源会复制未初始化的 0，导致 COPY 失效。 */
            }
            if (!dm->resident || !dm->symtab || !dm->strtab) {
                continue;
            }
            for (uint32_t s = 1; s < dm->symcount; s++) {
                elf64_sym_t *ds = &dm->symtab[s];
                if (ds->st_shndx == SHN_UNDEF) {
                    continue;   /* 跳过未定义；只取真正定义 */
                }
                if (ds->st_name && elf_sym_name_eq(dm, ds->st_name, nm)) {
                    src = dm->base + ds->st_value;   /* 定义方运行时地址 */
                    sz  = (size_t)ds->st_size;       /* 拷贝大小取自定义方 */
                    break;
                }
            }
            if (src) {
                break;
            }
        }
        if (src == 0 || sz == 0) {
            return false;   /* 找不到定义或大小未知，拒绝以防越界 */
        }
        elf_copy_user(as, addr, src, sz);
        return true;
    }

    if (symidx != 0) {
        elf64_sym_t *sym = &mod->symtab[symidx];
        if (sym->st_shndx == SHN_UNDEF) {
            const char *nm = mod->strtab + sym->st_name;
            S = elf_resolve_symbol(mods, n, nm);
            if (S == 0 && ELF_ST_BIND(sym->st_info) != STB_WEAK) {
                return false;   /* 强未定义符号无法解析 */
            }
        } else {
            S = mod->base + sym->st_value;
        }
    }

    uint64_t val;
    switch (type) {
    case R_X86_64_NONE:       return true;
    case R_X86_64_RELATIVE:   val = mod->base + (uint64_t)r->r_addend; break;
    case R_X86_64_GLOB_DAT:
    case R_X86_64_JUMP_SLOT:
    case R_X86_64_64:         val = S + (uint64_t)r->r_addend; break;
    default:                  return false;   /* 不支持的重定位类型 */
    }
    elf_write_user(as, addr, &val, 8);
    return true;
}

/* 对单模块应用 .rela.dyn 与 .rela.plt 全部重定位。 */
static bool elf_relocate(uint64_t as, elf_module_t *mod,
                         elf_module_t *mods, int n)
{
    for (uint32_t i = 0; i < mod->relacount; i++) {
        if (!elf_apply_reloc(as, mod, mods, n, &mod->rela[i])) {
            return false;
        }
    }
    for (uint32_t i = 0; i < mod->relapltcount; i++) {
        if (!elf_apply_reloc(as, mod, mods, n, &mod->rela_plt[i])) {
            return false;
        }
    }
    return true;
}

/* 广度优先加载所有已登记模块的 DT_NEEDED 依赖（.sl，约定 /LIB/<name>）。
 * 遍历中新加载的模块也会被继续处理（依赖的依赖）。 */
static bool elf_load_all_deps(struct task *t, uint64_t as)
{
    for (int i = 0; i < t->nmodules; i++) {
        elf_module_t *m = &t->modules[i];
        if (!m->dynamic) {
            continue;
        }
        for (elf64_dyn_t *d = m->dynamic; d->d_tag != DT_NULL; d++) {
            if ((uint64_t)d->d_tag != DT_NEEDED) {
                continue;
            }
            const char *libname = m->strtab + d->d_val;
            bool loaded = false;
            for (int k = 0; k < t->nmodules; k++) {
                if (strcmp(t->modules[k].name, libname) == 0) {
                    loaded = true;
                    break;
                }
            }
            if (loaded) {
                continue;
            }
            if (t->nmodules >= ELF_MODULE_MAX) {
                return false;
            }
            char path[256];
            const char *pre = "/LIB/";
            uint32_t pi = 0;
            for (const char *p = pre; *p && pi < sizeof(path) - 1; p++) {
                path[pi++] = *p;
            }
            for (const char *p = libname; *p && pi < sizeof(path) - 1; p++) {
                path[pi++] = *p;
            }
            path[pi] = '\0';
            int pl = (int)pi;
            if (pl <= 0) {
                return false;
            }
            const uint8_t *ldata = NULL;
            size_t llen = 0;
            uint8_t *lbuf = exec_read_file(path, (size_t)pl, &ldata, &llen);
            if (!lbuf) {
                kprintf("[elf] load_all_deps: exec_read_file('%s') failed\n", path);
                return false;
            }
            if (!elf_validate(lbuf, llen)) {
                kprintf("[elf] load_all_deps: elf_validate('%s') failed\n", path);
                kfree(lbuf);
                return false;
            }
            elf_load_result_t r;
            if (!elf_load(as, lbuf, llen, 0, NULL, 0, NULL, 0, 0, &r)) {
                kprintf("[elf] load_all_deps: elf_load('%s') failed\n", path);
                kfree(lbuf);
                return false;
            }
            elf_module_t *nm = &t->modules[t->nmodules];
            if (!elf_parse_dynamic(nm, lbuf, llen, r.base)) {
                kfree(lbuf);
                return false;
            }
            strncpy(nm->name, libname, sizeof(nm->name) - 1);
            nm->name[sizeof(nm->name) - 1] = '\0';
            nm->resident = 1;   /* 标记已加载（供 dlclose/重定位跳过空闲槽）*/
            nm->img_owned = 1;   /* lbuf 由 exec_read_file kmalloc，退出时释放 */
            t->nmodules++;
        }
    }
    return true;
}

bool elf_link_dynamic(struct task *t, uint64_t as,
                      const uint8_t *main_elf, size_t main_size,
                      uint64_t main_base, uint64_t main_entry)
{
    (void)main_entry;
    t->nmodules = 0;
    elf_module_t *mm = &t->modules[0];
    if (!elf_parse_dynamic(mm, main_elf, main_size, main_base)) {
        kprintf("[elf] link_dynamic: parse MAIN FAILED (size=%lu)\n", (unsigned long)main_size);
        return false;
    }
    strncpy(mm->name, "main", sizeof(mm->name) - 1);
    mm->name[sizeof(mm->name) - 1] = '\0';
    mm->resident = 1;
    t->nmodules = 1;

    if (!elf_load_all_deps(t, as)) {
        kprintf("[elf] link_dynamic: load_all_deps FAILED\n");
        return false;
    }
    for (int i = 0; i < t->nmodules; i++) {
        if (!t->modules[i].resident) {
            continue;   /* 跳过已卸载的空闲槽 */
        }
        if (!elf_relocate(as, &t->modules[i], t->modules, t->nmodules)) {
            kprintf("[elf] link_dynamic: relocate module %d ('%s') FAILED\n", i, t->modules[i].name);
            return false;
        }
    }
    return true;
}

int elf_dlopen(struct task *t, const char *path)
{
    size_t pl = strlen(path);
    const uint8_t *ldata = NULL;
    size_t llen = 0;

    /* 提取基名（去掉目录），用于「已加载去重」：同名库多次 dlopen 返回同一句柄。 */
    const char *bn = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') {
            bn = p + 1;
        }
    }

    /* 去重：同名库已加载则引用计数 +1，返回既有句柄（POSIX dlopen 语义），
     * 避免同一 .sl 被重复加载、重复占用地址空间。 */
    for (int k = 0; k < t->nmodules; k++) {
        if (t->modules[k].resident && strcmp(t->modules[k].name, bn) == 0) {
            t->modules[k].refcount++;
            return k;
        }
    }

    /* 选槽位：优先复用已卸载（非 resident）的空闲槽，否则在末尾新开（受 MAX 限制）。
     * 复用空闲槽可避免句柄表无限增长，dlclose 后该句柄即视为失效。 */
    int slot = -1, new_slot = 0;
    for (int k = 0; k < t->nmodules; k++) {
        if (!t->modules[k].resident) {
            slot = k;
            break;
        }
    }
    if (slot < 0) {
        if (t->nmodules >= ELF_MODULE_MAX) {
            return 0;
        }
        slot = t->nmodules;
        new_slot = 1;
        t->nmodules = slot + 1;   /* 预留范围；失败回退（resident 仍为 0，无害）*/
    }

    uint8_t *lbuf = exec_read_file(path, pl, &ldata, &llen);
    if (!lbuf) {
        if (new_slot) t->nmodules = slot;
        return 0;
    }
    if (!elf_validate(lbuf, llen)) {
        kfree(lbuf);
        if (new_slot) t->nmodules = slot;
        return 0;
    }
    elf_load_result_t r;
    if (!elf_load(t->cr3, lbuf, llen, 0, NULL, 0, NULL, 0, 0, &r)) {
        kfree(lbuf);
        if (new_slot) t->nmodules = slot;
        return 0;
    }
    elf_module_t *mod = &t->modules[slot];
    if (!elf_parse_dynamic(mod, lbuf, llen, r.base)) {
        elf_unmap_module(t->cr3, mod);   /* 解映射已加载的用户页 */
        kfree(lbuf);
        if (new_slot) t->nmodules = slot;
        return 0;
    }
    strncpy(mod->name, bn, sizeof(mod->name) - 1);
    mod->name[sizeof(mod->name) - 1] = '\0';
    mod->resident = 1;   /* 标记已加载（句柄 slot 生效）；refcount 已由 parse 置 1 */
    mod->img_owned = 1;   /* lbuf 由 exec_read_file kmalloc，退出时释放 */

    if (!elf_load_all_deps(t, t->cr3)) {
        elf_unmap_module(t->cr3, mod);
        kfree(mod->img);
        mod->img = NULL;
        mod->resident = 0;
        if (new_slot) t->nmodules = slot;
        return 0;
    }
    for (int i = 0; i < t->nmodules; i++) {
        if (!t->modules[i].resident) {
            continue;
        }
        if (!elf_relocate(t->cr3, &t->modules[i], t->modules, t->nmodules)) {
            elf_unmap_module(t->cr3, mod);
            kfree(mod->img);
            mod->img = NULL;
            mod->resident = 0;
            if (new_slot) t->nmodules = slot;
            return 0;
        }
    }
    return slot;   /* handle（0 保留给主程序）*/
}

uint64_t elf_dlsym(struct task *t, int h, const char *name)
{
    if (h < 0 || h >= t->nmodules) {
        return 0;
    }
    elf_module_t *m = &t->modules[h];
    if (!m->resident || !m->symtab || !m->strtab) {
        return 0;   /* 句柄失效（已 dlclose）或非法 */
    }
    for (uint32_t s = 1; s < m->symcount; s++) {
        elf64_sym_t *sym = &m->symtab[s];
        if (sym->st_shndx == SHN_UNDEF || sym->st_name == 0) {
            if (sym->st_info == 0 && sym->st_value == 0) {
                break;
            }
            continue;
        }
        if (elf_sym_name_eq(m, sym->st_name, name)) {
            return m->base + sym->st_value;
        }
    }
    return 0;
}

int elf_dlclose(struct task *t, int h)
{
    if (h <= 0 || h >= t->nmodules) {
        return -1;
    }
    elf_module_t *m = &t->modules[h];
    if (!m->resident) {
        return -1;   /* 已卸载或非法句柄 */
    }
    if (m->refcount > 0) {
        m->refcount--;
    }
    if (m->refcount > 0) {
        return 0;   /* 仍被其它引用持有（DT_NEEDED 依赖或重复 dlopen），暂不卸载 */
    }

    /* 引用归零：真正回收该模块占用的全部资源。 */
    kprintf("[elf] dlclose: reclaim module '%s' base=%p img=%p\n",
            m->name, (void *)m->base, (void *)m->img);
    elf_unmap_module(t->cr3, m);     /* 解映射用户页 + 释放物理页（pmm_decref）*/
    if (m->img && m->img_owned) {
        kfree(m->img);               /* 释放内核侧 ELF 副本（kmalloc）*/
        m->img = NULL;
    }
    m->img_owned = 0;
    m->symtab   = NULL;
    m->strtab   = NULL;
    m->dynamic  = NULL;
    m->rela     = NULL;
    m->rela_plt = NULL;
    m->symcount      = 0;
    m->relacount     = 0;
    m->relapltcount = 0;
    m->resident = 0;
    m->name[0]  = '\0';
    /* nmodules 保持不变：该槽位留作空闲可复用，句柄 h 此后视为失效。 */
    return 0;
}
