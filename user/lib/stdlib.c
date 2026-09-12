/*
 * user/lib/stdlib.c
 * 动态内存分配（基于内核 sys_brk 的堆区）+ 进程退出。
 *
 * 分配器设计（first-fit + 立即合并，真正可用，非 stub）：
 *   - 堆底由 sys_brk(0) 取得（内核已登记整块匿名 VMA，按需补页）。
 *   - 每个块头部 16 字节：{ size_t total; size_t free; }（total 含头部，
 *     free 为 1 表示该块空闲）。空闲块以单向链表串接（free 链）。
 *   - malloc：先扫描空闲链找 first-fit；找不到则向内核扩展 brk 开辟新块。
 *   - free：标记空闲，并与相邻（前向/后向）空闲块合并，避免碎片累积。
 */
#include "libc.h"

typedef struct block {
    size_t total;          /* 整块字节数（含头部） */
    size_t free;           /* 1 = 空闲，0 = 占用 */
    struct block *next;    /* 空闲链 next（仅空闲时有意义） */
} block_t;

#define BLK_HDR  (sizeof(block_t))
#define ALIGN8(n) (((n) + 7) & ~(size_t)7)
#define HEAP_PAGE 4096UL   /* 与内核页大小一致，仅用于初始化时确保基址页映射 */

static block_t *g_heap_base = NULL;   /* 第一个块 */
static block_t *g_free_head = NULL;   /* 空闲链头 */
static uint8_t  g_heap_init = 0;

static void heap_init(void)
{
    /* 取得当前 brk 作为堆底。注意：本进程之前可能已被其它代码（如测试
     * 套件的 sbrk 收缩）把堆基址所在的页 unmap 过，因此 base 这一页此刻
     * 未必在 VMA 内。必须先把 brk 向前扩展至少一页，让内核重新把
     * [base, base+PAGE) 纳入堆 VMA 并允许首次触碰补页，否则下面写
     * g_heap_base->total 会触发 #PF 杀进程。 */
    uintptr_t base = (uintptr_t)suki_syscall1(SYS_BRK, 0);
    if (!base) return;
    uintptr_t first_top = (base + HEAP_PAGE);
    uintptr_t got = (uintptr_t)suki_syscall1(SYS_BRK, first_top);
    if (got != first_top) return;   /* 内核拒绝扩展（OOM）：堆不可用 */
    g_heap_base = (block_t *)base;
    /* 第一个块覆盖整个 [base, base+PAGE)，全部空闲，供后续切分/合并 */
    g_heap_base->total = (size_t)(first_top - base);
    g_heap_base->free  = 1;
    g_heap_base->next  = NULL;
    g_free_head = g_heap_base;
    g_heap_init = 1;
}

/* 向内核申请至少 need 字节的新堆块，返回新块指针（已设为占用） */
static block_t *heap_extend(size_t need)
{
    uintptr_t cur = (uintptr_t)suki_syscall1(SYS_BRK, 0);
    size_t want = ALIGN8(need);
    uintptr_t new_brk = cur + want;
    uintptr_t got = (uintptr_t)suki_syscall1(SYS_BRK, new_brk);
    if (got != new_brk) {
        return NULL;    /* 内核拒绝扩展（OOM） */
    }
    block_t *b = (block_t *)cur;
    b->total = want;
    b->free  = 0;
    b->next  = NULL;
    return b;
}

void *malloc(size_t size)
{
    if (!size) return NULL;
    if (!g_heap_init) heap_init();
    if (!g_heap_init) return NULL;

    size_t need = ALIGN8(size) + BLK_HDR;

    /* first-fit 扫描空闲链 */
    block_t *prev = NULL;
    for (block_t *b = g_free_head; b; prev = b, b = b->next) {
        if (b->free && b->total >= need) {
            if (b->total >= need + BLK_HDR + 16) {
                /* 切分：剩余部分作为新空闲块放入空闲链 */
                block_t *split = (block_t *)((uintptr_t)b + need);
                split->total = b->total - need;
                split->free  = 1;
                split->next  = b->next;
                b->total = need;
                b->next = split;
            }
            b->free = 0;
            /* 从空闲链摘除（仅当 b 是链头时才需更新 head；切分后可能影响，统一处理） */
            if (prev) prev->next = b->next;
            else g_free_head = b->next;
            b->next = NULL;
            return (void *)((uintptr_t)b + BLK_HDR);
        }
    }

    /* 无合适空闲块：扩展堆 */
    block_t *nb = heap_extend(need);
    if (!nb) { errno = ENOMEM; return NULL; }
    return (void *)((uintptr_t)nb + BLK_HDR);
}

void free(void *ptr)
{
    if (!ptr) return;
    block_t *b = (block_t *)((uintptr_t)ptr - BLK_HDR);
    b->free = 1;
    b->next = NULL;

    uintptr_t brk = (uintptr_t)suki_syscall1(SYS_BRK, 0);

    /* 线性扫描整堆，合并相邻空闲块（前后向均处理） */
    block_t *cur = g_heap_base;
    while ((uintptr_t)cur < brk && cur->total >= BLK_HDR) {
        if (cur->free) {
            block_t *nxt = (block_t *)((uintptr_t)cur + cur->total);
            while ((uintptr_t)nxt > (uintptr_t)cur &&
                   (uintptr_t)nxt < brk &&
                   nxt->total >= BLK_HDR && nxt->free) {
                cur->total += nxt->total;
                nxt = (block_t *)((uintptr_t)cur + cur->total);
            }
        }
        cur = (block_t *)((uintptr_t)cur + cur->total);
    }

    /* 重建空闲链（单向，首插） */
    g_free_head = NULL;
    cur = g_heap_base;
    while ((uintptr_t)cur < brk && cur->total >= BLK_HDR) {
        if (cur->free) {
            cur->next = g_free_head;
            g_free_head = cur;
        } else {
            cur->next = NULL;
        }
        cur = (block_t *)((uintptr_t)cur + cur->total);
    }
}

void *calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    if (nmemb && size && total / nmemb != size) { errno = ENOMEM; return NULL; }
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size)
{
    if (!ptr) return malloc(size);
    if (!size) { free(ptr); return NULL; }
    block_t *b = (block_t *)((uintptr_t)ptr - BLK_HDR);
    size_t old_usable = b->total - BLK_HDR;
    if (old_usable >= size) return ptr;
    void *np = malloc(size);
    if (!np) return NULL;
    memcpy(np, ptr, old_usable);
    free(ptr);
    return np;
}

void exit(int code)
{
    suki_syscall1(SYS_EXIT_GROUP, (uint64_t)code);
    for (;;) { }
}

void _exit(int code)
{
    suki_syscall1(SYS_EXIT_GROUP, (uint64_t)code);
    for (;;) { }
}

void abort(void)
{
    suki_syscall1(SYS_EXIT_GROUP, 134);
    for (;;) { }
}

/* ===========================================================================
 * 环境变量（进程内维护）
 *   environ 指向一个 NULL 结尾的 char* 数组，每项形如 "NAME=VALUE"。
 *   初始 environ 由 crt0 通过 SYS_GETARGS 的 envp 设置；此处提供 CRUD 接口。
 *   注：内核当前没有 getenv/setenv syscall，环境变量完全在用户态维护，
 *   子进程（spawn）需要 env 时由调用方把 environ 显式传入。
 * =========================================================================== */
char **environ = NULL;

/* 惰性初始化 environ 为空数组（首次 setenv/putenv 时调用）。
 * 必须保证 environ 非空再进入涉及 environ[k] 遍历的逻辑，否则 NULL 解引用。 */
static void environ_init(void)
{
    if (environ) return;
    environ = (char **)malloc(sizeof(char *) * 2);
    if (environ) { environ[0] = NULL; environ[1] = NULL; }
}

static int env_find(const char *name, size_t *idx)
{
    if (!environ || !name) return 0;
    size_t nlen = strchr(name, '=') ? (size_t)(strchr(name, '=') - name)
                                    : strlen(name);
    for (size_t i = 0; environ[i]; i++) {
        const char *e = environ[i];
        size_t elen = strchr(e, '=') ? (size_t)(strchr(e, '=') - e) : strlen(e);
        if (elen == nlen && strncmp(e, name, nlen) == 0) {
            if (idx) *idx = i;
            return 1;
        }
    }
    return 0;
}

char *getenv(const char *name)
{
    size_t i;
    if (!name || !env_find(name, &i)) return NULL;
    const char *e = strchr(environ[i], '=');
    return e ? (char *)(e + 1) : (char *)"";
}

int setenv(const char *name, const char *value, int overwrite)
{
    if (!name || !*name || strchr(name, '=')) { errno = EINVAL; return -1; }
    environ_init();
    if (!environ) { errno = ENOMEM; return -1; }
    size_t i;
    if (env_find(name, &i)) {
        if (!overwrite) return 0;
        /* 覆盖已有项 */
        size_t need = strlen(name) + 1 + (value ? strlen(value) : 0) + 1;
        char *buf = (char *)malloc(need);
        if (!buf) { errno = ENOMEM; return -1; }
        snprintf(buf, need, "%s=%s", name, value ? value : "");
        free(environ[i]);
        environ[i] = buf;
        return 0;
    }
    /* 追加新项：重新分配 environ 数组（+2：新项 + NULL 哨兵，并保留已分配余量） */
    size_t count = 0;
    while (environ[count]) count++;
    char **ne = (char **)malloc(sizeof(char *) * (count + 2));
    if (!ne) { errno = ENOMEM; return -1; }
    for (size_t k = 0; k < count; k++) ne[k] = environ[k];
    size_t need = strlen(name) + 1 + (value ? strlen(value) : 0) + 1;
    ne[count] = (char *)malloc(need);
    if (!ne[count]) { free(ne); errno = ENOMEM; return -1; }
    snprintf(ne[count], need, "%s=%s", name, value ? value : "");
    ne[count + 1] = NULL;
    free(environ);
    environ = ne;
    return 0;
}

int unsetenv(const char *name)
{
    if (!name || !*name || strchr(name, '=')) { errno = EINVAL; return -1; }
    size_t i;
    if (!env_find(name, &i)) return 0;  /* 不存在不算错误 */
    free(environ[i]);
    size_t count = 0;
    while (environ[count]) count++;
    for (size_t k = i; k < count; k++) environ[k] = environ[k + 1];
    environ[count - 1] = NULL;
    return 0;
}

int putenv(char *string)
{
    if (!string || !strchr(string, '=')) { errno = EINVAL; return -1; }
    environ_init();
    if (!environ) { errno = ENOMEM; return -1; }
    size_t i;
    if (env_find(string, &i)) {
        environ[i] = string;   /* 直接接管调用方字符串 */
        return 0;
    }
    size_t count = 0;
    while (environ[count]) count++;
    char **ne = (char **)malloc(sizeof(char *) * (count + 2));
    if (!ne) { errno = ENOMEM; return -1; }
    for (size_t k = 0; k < count; k++) ne[k] = environ[k];
    ne[count] = string;
    ne[count + 1] = NULL;
    free(environ);
    environ = ne;
    return 0;
}

/* ===========================================================================
 * getopt / getopt_long  —— 命令行选项解析（bash/util-linux 语义子集）
 *   支持：短选项 -a -abc -aARG -a ARG；长选项 --long / --long=ARG；
 *   未知选项 opterr=1 时打印 "?unknown" 并返回 '?'；opterr=0 时静默返回 '?'；
 *   缺少必填参数返回 ':'（optstring 以 ':' 开头时）或 '?'；
 *   解析结束（遇到非选项参数或 "--"）返回 -1 且 optind 指向首个非选项参数。
 * =========================================================================== */
char *optarg = NULL;
int   optind = 1;
int   opterr = 1;
int   optopt = 0;

static const struct option *g_longopts = NULL;
static int  g_optpos = 0;   /* 当前 argv[optind] 内已解析到的字符偏移（支持 -abc 捆绑） */

static void getopt_err(const char *argv0, char ch, const char *msg)
{
    if (opterr) {
        char buf[128];
        size_t n = 0;
        const char *p = argv0 ? argv0 : "getopt";
        while (*p && n + 1 < sizeof(buf)) buf[n++] = *p++;
        const char *q = msg;
        while (*q && n + 1 < sizeof(buf)) buf[n++] = *q++;
        if (ch) {
            if (n + 4 < sizeof(buf)) {
                buf[n++] = ':'; buf[n++] = ' '; buf[n++] = '-'; buf[n++] = ch;
            }
        }
        if (n + 1 < sizeof(buf)) buf[n++] = '\n';
        buf[n] = '\0';
        u_print(buf);
    }
}

int getopt(int argc, char *const argv[], const char *optstring)
{
    g_longopts = NULL;
    if (optind >= argc) return -1;
    const char *arg = argv[optind];
    if (!arg || arg[0] != '-' || arg[1] == '\0')
        return -1;                       /* 非选项参数 */
    if (arg[1] == '-' && arg[2] == '\0') {
        optind++; g_optpos = 0;          /* "--" 结束符 */
        return -1;
    }
    if (arg[1] == '-') {
        /* 长选项（getopt 裸调用不处理长选项） */
        if (opterr) u_print("getopt: unexpected long option\n");
        optopt = 0;
        optind++; g_optpos = 0;
        return '?';
    }
    /* 处理 -abc 捆绑：g_optpos 指向下一个待解析字符 */
    if (g_optpos == 0) g_optpos = 1;
    char c = arg[g_optpos];
    if (c == '\0') { optind++; g_optpos = 0; return -1; }
    const char *p = strchr(optstring, c);
    if (!p) {
        optopt = c;
        getopt_err(argc > 0 ? argv[0] : "getopt", c, "invalid option");
        g_optpos++;
        if (arg[g_optpos] == '\0') { optind++; g_optpos = 0; }
        return '?';
    }
    if (p[1] == ':') {                    /* 需要参数 */
        if (arg[g_optpos + 1]) {
            optarg = (char *)(arg + g_optpos + 1);   /* -aARG 紧贴 */
            optind++; g_optpos = 0;
        } else if (optind + 1 < argc) {
            optind += 2; g_optpos = 0;      /* 跨过选项及其参数两个元素 */
            optarg = argv[optind - 1];
        } else {
            optopt = c;
            getopt_err(argc > 0 ? argv[0] : "getopt", c, "option requires an argument");
            optind++; g_optpos = 0;
            return (optstring[0] == ':') ? ':' : '?';
        }
        return c;
    }
    /* 无参数选项，可能粘多个：-abc */
    g_optpos++;
    if (arg[g_optpos] == '\0') { optind++; g_optpos = 0; }
    return c;
}

int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex)
{
    g_longopts = longopts;
    if (optind >= argc) return -1;
    const char *arg = argv[optind];
    if (!arg || arg[0] != '-' || arg[1] == '\0') return -1;
    if (arg[1] == '-' && arg[2] == '\0') { optind++; return -1; }  /* "--" */

    if (arg[1] == '-') {
        /* 长选项：--name 或 --name=VAL */
        const char *name = arg + 2;
        const char *eq = strchr(name, '=');
        size_t nlen = eq ? (size_t)(eq - name) : strlen(name);
        for (int i = 0; longopts && longopts[i].name; i++) {
            if (strncmp(longopts[i].name, name, nlen) == 0 &&
                strlen(longopts[i].name) == nlen) {
                if (longindex) *longindex = i;
                if (longopts[i].has_arg == required_argument) {
                    if (eq) optarg = (char *)(eq + 1);
                    else if (optind + 1 < argc) { optind++; optarg = argv[optind]; }
                    else {
                        if (opterr) { char b[160]; size_t n=0; const char*m="getopt_long: option '--"; while(*m&&n+1<sizeof(b))b[n++]=*m++; const char*nm=name; while(*nm&&n+1<sizeof(b))b[n++]=*nm++; const char*m2="' requires an argument\n"; while(*m2&&n+1<sizeof(b))b[n++]=*m2++; b[n]='\0'; u_print(b); }
                        optind++; optopt = longopts[i].val; return '?';
                    }
                } else if (eq && longopts[i].has_arg == no_argument) {
                    if (opterr) { char b[160]; size_t n=0; const char*m="getopt_long: option '--"; while(*m&&n+1<sizeof(b))b[n++]=*m++; const char*nm=name; while(*nm&&n+1<sizeof(b))b[n++]=*nm++; const char*m2="' doesn't allow an argument\n"; while(*m2&&n+1<sizeof(b))b[n++]=*m2++; b[n]='\0'; u_print(b); }
                    optind++; optopt = longopts[i].val; return '?';
                } else {
                    optarg = NULL;
                }
                optind++;
                if (longopts[i].flag) { *longopts[i].flag = longopts[i].val; return 0; }
                return longopts[i].val;
            }
        }
        if (opterr) { char b[140]; size_t n=0; const char*m="getopt_long: unrecognized option '--"; while(*m&&n+1<sizeof(b))b[n++]=*m++; const char*nm=name; while(*nm&&n+1<sizeof(b))b[n++]=*nm++; const char*m2="'\n"; while(*m2&&n+1<sizeof(b))b[n++]=*m2++; b[n]='\0'; u_print(b); }
        optind++; optopt = 0; return '?';
    }
    /* 退化为短选项解析（复用 getopt 主体） */
    return getopt(argc, argv, optstring);
}

/* ========================================================================== */
/* qsort：标准快排（Lomuto 分区 + 显式栈，迭代式），供 FreeType 等使用。        */
/* ========================================================================== */
static void tq_swap(char *a, char *b, size_t sz)
{
    char t;
    for (size_t i = 0; i < sz; i++) { t = a[i]; a[i] = b[i]; b[i] = t; }
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *))
{
    if (!base || nmemb < 2 || size == 0 || !compar) return;
    /* 显式栈模拟递归（避免 freestanding 深层递归栈风险） */
    struct { char *lo, *hi; } stack[64];
    int sp = 0;
    stack[sp].lo = (char*)base;
    stack[sp].hi = (char*)base + (nmemb - 1) * size;
    sp++;

    while (sp > 0) {
        sp--;
        char *lo = stack[sp].lo;
        char *hi = stack[sp].hi;
        while (lo < hi) {
            /* 小数组用插入排序（更快且省栈） */
            if ((size_t)(hi - lo) / size < 16) {
                for (char *i = lo + size; i <= hi; i += size) {
                    char *j = i;
                    while (j > lo && compar(j - size, j) > 0) {
                        tq_swap(j - size, j, size);
                        j -= size;
                    }
                }
                break;
            }
            /* Lomuto 分区，pivot 取末元素 */
            char *pivot = hi;
            char *i = lo;
            for (char *j = lo; j < hi; j += size) {
                if (compar(j, pivot) <= 0) {
                    if (i != j) tq_swap(i, j, size);
                    i += size;
                }
            }
            if (i != pivot) tq_swap(i, pivot, size);
            /* i 为 pivot 最终位置，分 [lo, i-size) 与 (i+size, hi] */
            char *p = i;            /* pivot 位置 */
            if (p > lo)  { stack[sp].lo = lo; stack[sp].hi = p - size; sp++; }
            if (p < hi)  { stack[sp].lo = p + size; stack[sp].hi = hi; sp++; }
            break; /* 本层处理完，回到 while 处理新入栈区间 */
        }
        if (sp >= (int)(sizeof(stack)/sizeof(stack[0]))) break; /* 防御 */
    }
}

/* 标准二分查找（要求 base 已按 compar 升序排列）；命中返回元素指针，否则 NULL。 */
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *))
{
    if (!key || !base || size == 0 || !compar) return NULL;
    size_t lo = 0, hi = nmemb;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const char *p = (const char *)base + mid * size;
        int c = compar(key, p);
        if (c == 0) return (void *)p;
        if (c < 0) hi = mid;
        else       lo = mid + 1;
    }
    return NULL;
}
