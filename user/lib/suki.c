/*
 * user/lib/suki.c
 * -----------------------------------------------------------------------------
 * SukiOS 用户态运行库实现（最小字符串/输出工具）。
 */
#include "suki.h"

size_t u_strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

int u_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int u_strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}

void *u_memcpy(void *d, const void *s, size_t n)
{
    uint8_t *dd = (uint8_t *)d;
    const uint8_t *ss = (const uint8_t *)s;
    while (n--) {
        *dd++ = *ss++;
    }
    return d;
}

void *u_memset(void *d, int c, size_t n)
{
    uint8_t *dd = (uint8_t *)d;
    while (n--) {
        *dd++ = (uint8_t)c;
    }
    return d;
}

int u_memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *aa = (const uint8_t *)a;
    const uint8_t *bb = (const uint8_t *)b;
    while (n--) {
        if (*aa != *bb) return (int)*aa - (int)*bb;
        aa++; bb++;
    }
    return 0;
}

/* ---- 标准 C 内存原语（供 minimp3 等库链接） ----
 * 注意：不要用 __builtin_* 递归实现；直接逐字节循环，freestanding 安全。 */
void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *dd = (uint8_t *)d;
    const uint8_t *ss = (const uint8_t *)s;
    while (n--) {
        *dd++ = *ss++;
    }
    return d;
}

void *memset(void *d, int c, size_t n)
{
    uint8_t *dd = (uint8_t *)d;
    while (n--) {
        *dd++ = (uint8_t)c;
    }
    return d;
}

/* 处理重叠区间：src<dst 时从尾部倒序拷贝 */
void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *dd = (uint8_t *)d;
    const uint8_t *ss = (const uint8_t *)s;
    if (dd == ss || n == 0) {
        return d;
    }
    if (dd < ss) {
        while (n--) {
            *dd++ = *ss++;
        }
    } else {
        dd += n;
        ss += n;
        while (n--) {
            *--dd = *--ss;
        }
    }
    return d;
}

/* FatFs ff.c 使用的标准 C memcmp（freestanding 下需自行提供强符号）。 */
int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *aa = (const uint8_t *)a;
    const uint8_t *bb = (const uint8_t *)b;
    while (n--) {
        if (*aa != *bb) {
            return (int)*aa - (int)*bb;
        }
        aa++; bb++;
    }
    return 0;
}

void u_print(const char *s)
{
    sys_debug_write(s, u_strlen(s));
}

void u_printn(const char *s, size_t n)
{
    sys_debug_write(s, n);
}

/*
 * u_utoa_s：长度安全的十进制转换（M17 接口收尾）。
 * - size 为 buf 总容量（含 NUL）。输出最多写 size-1 个字符 + NUL。
 * - size == 0 时不写任何字节，返回 buf（调用方错误但绝不越界）。
 * - 容量不足以容纳完整数字时，输出被截断为高位在前的前缀并置 NUL
 *   （uint64 十进制最长 20 位，size >= 21 即永不截断）。
 */
char *u_utoa_s(uint64_t v, char *buf, size_t size)
{
    if (size == 0) {
        return buf;
    }
    char tmp[21];               /* uint64 十进制最长 20 位 + 冗余 */
    int i = 0;
    if (v == 0) {
        tmp[i++] = '0';
    }
    while (v && i < 20) {
        tmp[i++] = (char)('0' + v % 10);
        v /= 10;
    }
    size_t j = 0;
    while (i > 0 && j + 1 < size) {     /* 恒预留 1 字节 NUL */
        buf[j++] = tmp[--i];
    }
    buf[j] = '\0';
    return buf;
}

/* 兼容包装：旧接口约定调用方缓冲至少 24 字节（现有调用点均为 char[24]）。
 * 新代码一律使用 u_utoa_s。 */
char *u_utoa(uint64_t v, char *buf)
{
    return u_utoa_s(v, buf, 24);
}

/* ---- strchr：FatFs ff.c 用于非法字符检查（FTN/SFN 校验） ---- */
char *strchr(const char *s, int c)
{
    char ch = (char)c;
    while (*s) {
        if (*s == ch) return (char *)s;
        s++;
    }
    return (ch == '\0') ? (char *)s : NULL;
}

/* ---- 极简页粒度堆（供 FatFs ff_memalloc / ff_memfree，单线程 FS_SERVER） ----
 * FatFs 在 FF_USE_LFN>=1 时通过 ff_memalloc 申请 LFN 工作缓冲（每次
 * open/opendir 一处，几十~几百字节），用完 ff_memfree 归还。这里用内核
 * sys_mmap 按页领取内存，维护一个空闲块链表（首次适配）做回收，避免反复
 * mmap 造成内核映射泄漏。无 libc，故自行实现。 */
#define SUKI_HEAP_PAGES   32          /* 预领 32 页 = 128 KiB，足够 LFN 缓冲 */
#define SUKI_HEAP_BYTES   (SUKI_HEAP_PAGES * 4096u)
#define SUKI_HEAP_ALIGN   16u

typedef struct heap_node {
    struct heap_node *next;
    uint32_t size;            /* 本块可用字节数（不含头） */
    uint32_t used;            /* 0=空闲, 1=已用 */
} heap_node_t;

static uint8_t  g_heap[SUKI_HEAP_BYTES] __attribute__((aligned(4096)));
static heap_node_t *g_heap_free = NULL;

static void heap_init(void)
{
    if (g_heap_free) return;
    heap_node_t *b = (heap_node_t *)g_heap;
    b->next = NULL;
    b->size = SUKI_HEAP_BYTES - (uint32_t)sizeof(heap_node_t);
    b->used = 0;
    g_heap_free = b;
}

/* 向上对齐到 SUKI_HEAP_ALIGN */
static uint32_t heap_align_up(uint32_t v)
{
    return (v + SUKI_HEAP_ALIGN - 1) & ~(SUKI_HEAP_ALIGN - 1);
}

void *ff_memalloc(unsigned int msize)
{
    heap_init();
    uint32_t need = heap_align_up((uint32_t)msize);
    heap_node_t *prev = NULL;
    heap_node_t *cur = g_heap_free;
    while (cur) {
        if (!cur->used && cur->size >= need) {
            /* 若剩余空间足够切出一个新空闲块，则拆分 */
            uint32_t remain = cur->size - need;
            if (remain > sizeof(heap_node_t) + SUKI_HEAP_ALIGN) {
                heap_node_t *nb = (heap_node_t *)((uint8_t *)cur +
                                                  sizeof(heap_node_t) + need);
                nb->next = cur->next;
                nb->size = remain - (uint32_t)sizeof(heap_node_t);
                nb->used = 0;
                cur->next = nb;
                cur->size = need;
            }
            cur->used = 1;
            return (void *)((uint8_t *)cur + sizeof(heap_node_t));
        }
        prev = cur;
        cur = cur->next;
    }
    return NULL;   /* 池耗尽 */
}

void ff_memfree(void *mblock)
{
    if (!mblock) return;
    heap_node_t *cur = (heap_node_t *)((uint8_t *)mblock - sizeof(heap_node_t));
    cur->used = 0;
    /* 与相邻空闲块合并（简单向后合并：若 next 空闲则并入） */
    heap_node_t *n = cur->next;
    if (n && !n->used) {
        cur->size += (uint32_t)sizeof(heap_node_t) + n->size;
        cur->next = n->next;
    }
}
