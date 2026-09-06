/*
 * user/lib/arch/cc.h
 * -----------------------------------------------------------------------------
 * lwIP 2.2.1 的编译器/平台适配头（lwip/arch.h 会 #include "arch/cc.h"）。
 *
 * 说明：x86_64-sukios-elf 是 freestanding 交叉工具链，但 GCC 仍提供
 * stddef.h/stdint.h/inttypes.h/limits.h 等 freestanding 标准头，故
 * LWIP_NO_STDDEF_H / LWIP_NO_STDINT_H / LWIP_NO_INTTYPES_H / LWIP_NO_LIMITS_H
 * 均保持 0（使用工具链版本），本文件只提供 lwIP 规范要求的：
 *   - 结构体打包宏（PACK_STRUCT_*）：GCC 用 __attribute__((packed))；
 *   - 字节序（x86_64 为小端）；
 *   - 诊断输出与断言（LWIP_PLATFORM_DIAG / LWIP_PLATFORM_ASSERT）；
 *   - 随机数（LWIP_RAND）。
 */
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>
#include <stddef.h>

/* ---- 结构体打包（协议头必须紧凑布局，不允许编译器插入填充） ---- */
#define PACK_STRUCT_FIELD(x)    x __attribute__((packed))
#define PACK_STRUCT_STRUCT      __attribute__((packed))
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END

/* ---- 临界区保护类型（NO_SYS=1：单线程，保护为 no-op） ----
 * lwIP 在 SYS_LIGHTWEIGHT_PROT=0 时使用宏版 sys_arch_protect/unprotect（引用
 * sys_prot_t 类型但不调用函数），因此只需提供类型，无需实现 protect/unprotect。 */
typedef uint32_t sys_prot_t;

/* ---- 字节序：x86_64 小端 ---- */
#define BYTE_ORDER              LITTLE_ENDIAN

/* ---- 诊断/断言 ----
 * 用户态用 u_print（见 user/lib/suki.h）。断言失败打印后进入死循环：
 * 网络服务是独立 Ring3 任务，死循环不会拖垮内核（内核调度器不受影响）。 */
extern void u_print(const char *s);

#define LWIP_PLATFORM_DIAG(x)   do { (void)(x); } while (0)
#define LWIP_PLATFORM_ASSERT(x) \
    do { u_print("lwIP ASSERT: "); u_print(x); u_print("\n"); \
         for (;;) { } } while (0)

/* ---- 随机数：xorshift32（由网络服务提供 lwip_port_rand()，无 libc rand 依赖） ---- */
#define LWIP_RAND()             ((u32_t)lwip_port_rand())
extern unsigned int lwip_port_rand(void);

/* ---- 格式化前缀（诊断关闭时不使用） ---- */
#define LWIP_ERRNO_STDINCLUDE   0

#endif /* LWIP_ARCH_CC_H */
