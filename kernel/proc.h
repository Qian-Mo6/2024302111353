/* ============ lab2 · proc 定义 ============
 * 最小进程机制:够用即可(装下运行 sh、fork 子进程、wait 回收)
 * lab2 共用全局页表 g_kpgtbl,每进程独立 prog_page/stack_page
 * fork/exit 时用 user_remap 切换 VA 0x0/0x1000 到当前进程的页
 * ======================================== */
#ifndef PROC_H
#define PROC_H

#include "types.h"

/* riscv.h 无 C 侧 include 守卫,不能在此包含(会重定义 inline 函数)
 * 仅本地守卫 PGSIZE,供页对齐使用 */
#ifndef PGSIZE
#define PGSIZE 4096
#endif

#define NPROC 64              /* 进程表最大容量 */

enum procstate { UNUSED, USED, RUNNING, RUNNABLE, ZOMBIE };

struct proc {
    enum procstate state;
    int pid;
    struct proc *parent;

    struct trapframe *trapframe;  /* 指向 trapframe 内存(由 allocproc 分配) */

    /* 用户内存:每进程一份程序页 + 栈页(页对齐)
     * fork 时复制父进程页;sys_exec 时加载新程序到 prog_page
     * user_remap 把 VA 0x0 → prog_page, VA 0x1000 → stack_page */
    char prog_page[PGSIZE]  __attribute__((aligned(PGSIZE)));
    char stack_page[PGSIZE] __attribute__((aligned(PGSIZE)));
    uint64 mem_size;
};

#endif

/* 函数原型 */
struct proc *myproc(void);
struct proc *allocproc(void);
struct proc *fork(void);       /* Stage6: 创建子进程,立即切换到子进程运行 */
int wait(void);                /* Stage6: 等待 ZOMBIE 子进程并回收 */
void freeproc(struct proc *p);
