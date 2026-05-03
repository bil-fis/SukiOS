#ifndef SUKIOS_PROC_H
#define SUKIOS_PROC_H

#include <stdint.h>
#include <stddef.h>

#define PROC_READY      0
#define PROC_RUNNING    1
#define PROC_BLOCKED    2
#define PROC_ZOMBIE     3

#define KERNEL_STACK_SIZE   8192
#define USER_STACK_SIZE     (4 * 4096)   /* 16 KB 用户栈 */
#define USER_STACK_TOP      0x7FFFE000   /* 2GB - 8KB，安全且对齐 */

#define MAX_FD              16

/* 文件描述符 */
typedef struct {
    void *vnode;
    uint32_t mode;
    uint32_t offset;
} fd_t;

/* 进程控制块 (PCB) */
typedef struct pcb {
    uint64_t        rsp;            /* 内核栈指针（指向 interrupt_frame 的 r15） */
    uint64_t        cr3;            /* 页表物理基址 */
    uint64_t        pid;
    uint8_t         state;
    uint32_t        time_slice;
    struct pcb      *next;

    /* 用户态信息 */
    uint64_t        user_stack;     /* 用户栈顶虚拟地址 */
    uint64_t        entry_point;    /* 用户程序入口虚拟地址 */
    fd_t            fds[MAX_FD];    /* 文件描述符表 */
    uint8_t         *kernel_stack;  /* 内核栈虚拟地址 */
} pcb_t;

extern pcb_t *g_current_proc;

/* 调度器接口 */
void   scheduler_init(void);
pcb_t* proc_create_from_elf(const void *elf_data, size_t size);
void   scheduler_start(void);
void   schedule(void);
void   switch_to_proc(pcb_t *next);

/* 由中断入口保存的当前进程栈指针
   在 idt_stubs.asm 里使用 */
void set_current_rsp(uint64_t rsp);

/* ticks */
uint64_t get_ticks(void);

#endif