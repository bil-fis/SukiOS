/*
 * kernel/kdr/kdr.c
 * -----------------------------------------------------------------------------
 * SukiOS 内核模块加载器（.kdr，Ring0 内核驱动）
 *
 * 设计要点：
 *   1. .kdr 是交叉 gcc 编成的 ET_DYN 共享对象（DSO），其引用的内核 API
 *      （kprintf/kmalloc/driver_register 等）在链接期 UNDEF，运行时由 ksymtab 解析。
 *   2. 加载器把每个 PT_LOAD 段拷贝到内核虚拟地址：物理页由 pmm 分配（可不连续），
 *      但映射到一段【连续】的预留虚拟区 KMODULE_BASE（避免跨页撕裂）。
 *   3. 页权限按段：先全部以 RW+NX 映射（便于写段、清 BSS、应用重定位），写完后再
 *      把 PF_X 代码段页重新映射为 RX（W^X），使模块代码可在 Ring0 执行。
 *   4. 解析 .dynsym/.dynstr/.rela.dyn/.rela.plt，对每项重定位：
 *        - 模块内符号（st_shndx != SHN_UNDEF）→ base + st_value；
 *        - 外部符号（SHN_UNDEF）→ ksym_lookup(名字)（内核导出地址）；
 *      支持 R_X86_64_64 / GLOB_DAT / JUMP_SLOT / RELATIVE / PC32 / PLT32。
 *   5. 解析导出的 kdr_init，调用之；kdr_init 内部调用 driver_register() 注册驱动，
 *      从而参与「驱动—设备」双向匹配（一套驱动驱动多个设备）。
 *
 * 说明：加载发生在内核启动早期（内存子系统就绪后），此时 CR3 为内核 PML4，
 * 模块映射在内核高半区，所有 Ring0 上下文（中断/系统调用/初始化）均可见。
 */
#include <kernel/types.h>
#include <kernel/console.h>
#include <kernel/spinlock.h>
#include <mm/kmalloc.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <kernel/elf.h>
#include <kernel/ksym.h>
#include <kernel/string.h>
#include <kernel/multiboot2.h>
#include <sukios/kdr.h>

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096UL
#endif
#ifndef R_X86_64_PLT32
#define R_X86_64_PLT32 4     /* 与 elf.h 一致的 PLT 32 位 PC 相对重定位类型 */
#endif

/* 内核模块专用连续虚拟区（高于内核映像与 kmalloc 堆，物理 RAM 通常远不及此） */
#define KMODULE_BASE 0xFFFFC00000000000ULL

typedef struct kdr_mod {
    const char     *name;
    void           *base;
    size_t          size;
    kdr_exit_fn     exit;
    struct kdr_mod *next;
} kdr_mod_t;

static kdr_mod_t *g_kdrs = NULL;
static uint64_t   g_kmod_cursor = KMODULE_BASE;
static spinlock_t g_kdr_lock;

/* DSO 动态节信息 */
struct dyninfo {
    const elf64_sym_t *symtab;
    size_t             nsym;
    const char        *strtab;
    const elf64_rela_t *rela;     /* .rela.dyn */
    size_t             nrela;
    const elf64_rela_t *rela_plt; /* .rela.plt */
    size_t             nrela_plt;
};

static bool parse_dynamic(const uint8_t *img, const elf64_hdr_t *eh, struct dyninfo *di)
{
    memset(di, 0, sizeof(*di));
    const elf64_phdr_t *ph = (const elf64_phdr_t *)(img + eh->e_phoff);
    const elf64_dyn_t  *dyn = NULL;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) {
            dyn = (const elf64_dyn_t *)(img + ph[i].p_offset);
            break;
        }
    }
    if (!dyn)
        return false;

    for (const elf64_dyn_t *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_SYMTAB:  di->symtab = (const elf64_sym_t *)(img + d->d_val); break;
        case DT_STRTAB:  di->strtab = (const char *)(img + d->d_val); break;
        case DT_RELA:    di->rela    = (const elf64_rela_t *)(img + d->d_val); break;
        case DT_RELASZ:  di->nrela   = d->d_val / sizeof(elf64_rela_t); break;
        case DT_JMPREL:  di->rela_plt = (const elf64_rela_t *)(img + d->d_val); break;
        case DT_PLTRELSZ:di->nrela_plt = d->d_val / sizeof(elf64_rela_t); break;
        /* 符号数量：构建时强制 --hash-style=sysv，使 DT_HASH[1] = nsyms */
        case DT_HASH: {
            const uint32_t *h = (const uint32_t *)(img + d->d_val);
            di->nsym = h[1];
            break;
        }
        default: break;
        }
    }
    return true;
}

/* 解析单个重定位项的符号地址（模块内 / 内核导出） */
static uint64_t resolve_sym(struct dyninfo *di, uint64_t symidx, uint64_t base)
{
    const elf64_sym_t *s = &di->symtab[symidx];
    if (s->st_shndx == SHN_UNDEF) {
        const char *nm = di->strtab + s->st_name;
        uint64_t v = ksym_lookup(nm);
        if (!v)
            kprintf("[kdr]   UNRESOLVED symbol '%s'\n", nm);
        return v;
    }
    return base + s->st_value;
}

static void do_rela(struct dyninfo *di, const elf64_rela_t *rela, size_t n, uint64_t base)
{
    for (size_t i = 0; i < n; i++) {
        const elf64_rela_t *r = &rela[i];
        uint32_t type   = ELF64_R_TYPE(r->r_info);
        uint32_t symidx = ELF64_R_SYM(r->r_info);
        uint64_t addr = base + r->r_offset;
        uint64_t S = 0;
        if (symidx != 0)
            S = resolve_sym(di, symidx, base);

        switch (type) {
        case R_X86_64_NONE:
            break;
        case R_X86_64_64:
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT:
            *(uint64_t *)addr = S + r->r_addend;
            break;
        case R_X86_64_RELATIVE:
            *(uint64_t *)addr = base + r->r_addend;
            break;
        case R_X86_64_PC32:
        case R_X86_64_PLT32:
            *(uint32_t *)addr = (uint32_t)(S + r->r_addend - addr);
            break;
        default:
            kprintf("[kdr]   unsupported reloc type %u @%p\n", type, (void *)addr);
            break;
        }
    }
}

/* 在 .dynsym 中按名字查找「已定义」导出符号（用于定位 kdr_init/kdr_exit） */
static uint64_t find_exported(struct dyninfo *di, const char *target, uint64_t base)
{
    for (size_t i = 0; i < di->nsym; i++) {
        const elf64_sym_t *s = &di->symtab[i];
        if (s->st_shndx == SHN_UNDEF)
            continue;
        if (strcmp(di->strtab + s->st_name, target) == 0)
            return base + s->st_value;
    }
    return 0;
}

static int kdr_load(const uint8_t *img, size_t sz, const char *name)
{
    (void)sz;
    const elf64_hdr_t *eh = (const elf64_hdr_t *)img;
    if (!(eh->e_ident[0] == 0x7f && eh->e_ident[1] == 'E' &&
          eh->e_ident[2] == 'L' && eh->e_ident[3] == 'F') ||
        eh->e_type != ET_DYN || eh->e_machine != EM_X86_64) {
        kprintf("[kdr] %s: not a valid x86-64 DSO\n", name);
        return -1;
    }

    /* 计算需映射的虚拟范围 */
    const elf64_phdr_t *ph = (const elf64_phdr_t *)(img + eh->e_phoff);
    uint64_t vmax = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) {
            uint64_t end = ph[i].p_vaddr + ph[i].p_memsz;
            if (end > vmax) vmax = end;
        }
    }
    if (vmax == 0) {
        kprintf("[kdr] %s: no loadable segments\n", name);
        return -1;
    }
    uint64_t npages = (vmax + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t *paddrs = kmalloc(npages * sizeof(uint64_t));
    if (!paddrs) {
        kprintf("[kdr] %s: out of memory\n", name);
        return -1;
    }

    uint64_t kpml4 = vmm_kernel_pml4();
    uint64_t base  = g_kmod_cursor;
    g_kmod_cursor += npages * PAGE_SIZE;

    /* 1) 分配物理页并映射为 RW+NX（便于写入与重定位） */
    for (uint64_t pg = 0; pg < npages; pg++) {
        uint64_t p = (uint64_t)pmm_alloc_page();
        paddrs[pg] = p;
        uint64_t va = base + pg * PAGE_SIZE;
        vmm_unmap_page(kpml4, va);
        vmm_map_page(kpml4, va, p, PTE_PRESENT | PTE_WRITE | PTE_NX);
    }

    /* 2) 拷贝段数据 + 清零 BSS */
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uint8_t *dst = (uint8_t *)(base + ph[i].p_vaddr);
        memcpy(dst, img + ph[i].p_offset, ph[i].p_filesz);
        if (ph[i].p_memsz > ph[i].p_filesz)
            memset(dst + ph[i].p_filesz, 0, ph[i].p_memsz - ph[i].p_filesz);
    }

    /* 3) 解析动态节并应用重定位 */
    struct dyninfo di;
    if (!parse_dynamic(img, eh, &di)) {
        kprintf("[kdr] %s: no PT_DYNAMIC (nsym=%u), skip relocations\n", name, (uint32_t)di.nsym);
    } else {
        do_rela(&di, di.rela, di.nrela, base);
        do_rela(&di, di.rela_plt, di.nrela_plt, base);
    }

    /* 4) 代码段（PF_X）页重新映射为 RX（W^X） */
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (!(ph[i].p_flags & PF_X)) continue;
        uint64_t seg_start = ph[i].p_vaddr & ~(PAGE_SIZE - 1);
        uint64_t seg_end   = ph[i].p_vaddr + ph[i].p_memsz;
        for (uint64_t a = seg_start; a < seg_end; a += PAGE_SIZE) {
            uint64_t pg = a / PAGE_SIZE;
            uint64_t va = base + pg * PAGE_SIZE;
            vmm_unmap_page(kpml4, va);
            vmm_map_page(kpml4, va, paddrs[pg], PTE_PRESENT); /* RX */
        }
    }

    /* 5) 调用模块入口 kdr_init */
    uint64_t init_addr = di.strtab ? find_exported(&di, "kdr_init", base) : 0;
    uint64_t exit_addr = di.strtab ? find_exported(&di, "kdr_exit", base) : 0;

    kdr_mod_t *m = kmalloc(sizeof(kdr_mod_t));
    if (m) {
        m->name = name;
        m->base = (void *)base;
        m->size = npages * PAGE_SIZE;
        m->exit = (kdr_exit_fn)exit_addr;
        spin_lock(&g_kdr_lock);
        m->next = g_kdrs; g_kdrs = m;
        spin_unlock(&g_kdr_lock);
    }

    kprintf("[kdr] loaded '%s' @%p size=%uKiB pages=%u init=%s\n",
            name, (void *)base, (uint32_t)(npages * PAGE_SIZE / 1024),
            (uint32_t)npages, init_addr ? "yes" : "NO");

    if (init_addr)
        ((kdr_init_fn)init_addr)();
    else
        kprintf("[kdr] %s: warning: kdr_init not found, driver not registered\n", name);

    return 0;
}

/* 遍历引导模块，加载所有标记为 .kdr 的模块 */
int kdr_load_all(void)
{
    spinlock_init(&g_kdr_lock, "kdr-loader");
    kprintf("[kdr] loading kernel drivers from boot modules...\n");
    int n = 0;
    for (int i = 0; i < g_boot.nmods; i++) {
        if (!g_boot.mods[i].is_kdr)
            continue;
        /* GRUB 把模块放在低位物理内存，PMM 之后可能回收该页，故先拷到内核堆再解析 */
        const uint8_t *src = (const uint8_t *)PHYS_TO_VIRT(g_boot.mods[i].phys);
        uint8_t *buf = kmalloc(g_boot.mods[i].size);
        if (!buf) {
            kprintf("[kdr]   '%s': out of memory\n", g_boot.mods[i].name);
            continue;
        }
        memcpy(buf, src, g_boot.mods[i].size);
        kprintf("[kdr]   module '%s' phys=%p size=%u first4=%02x%02x%02x%02x\n",
                g_boot.mods[i].name, (void *)(uintptr_t)g_boot.mods[i].phys,
                g_boot.mods[i].size,
                buf[0], buf[1], buf[2], buf[3]);
        kdr_load(buf, g_boot.mods[i].size, g_boot.mods[i].name);
        kfree(buf);   /* 段已拷入内核模块映射页，原副本可释放 */
        n++;
    }
    kprintf("[kdr] loaded %d kernel driver module(s)\n", n);
    return n;
}
