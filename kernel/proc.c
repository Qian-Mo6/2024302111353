/* ============ lab2 阶段一 · proc.c 进程管理 ============
 * 最小实现:全局 proc 表 + allocproc + myproc
 * lab2 单核单进程,myproc 简化返回 proc[0]
 * lab4 实现 scheduler 后会重构 myproc
 * ======================================== */
#include "types.h"
#include "proc.h"
#include "riscv.h"
#include "memlayout.h"

struct proc proc[NPROC];  /* 全局进程表 */
struct proc *curproc;     /* 当前运行进程(lab2 单核简化) */

/* trapframe 内存池:每进程一份,静态分配
 * lab3 改 kalloc 动态分配 */
static struct trapframe tf_pool[NPROC] __attribute__((aligned(PGSIZE)));

/* myproc:返回当前 CPU 上运行的进程
 * lab2 单核简化:直接返回 curproc */
struct proc *
myproc(void)
{
    return curproc;
}

/* allocproc:从 proc[] 表找一个 UNUSED 槽,初始化后返回 */
struct proc *
allocproc(void)
{
    struct proc *p;
    static int nextpid = 1;

    for (p = proc; p < &proc[NPROC]; p++) {
        if (p->state == UNUSED) {
            /* 分配 trapframe */
            p->trapframe = &tf_pool[p - proc];
            /* 清空 trapframe */
            for (int i = 0; i < (int)sizeof(*p->trapframe) / 8; i++)
                ((uint64 *)p->trapframe)[i] = 0;

            p->pid = nextpid++;
            p->state = USED;
            p->parent = 0;
            p->mem_size = 0;
            return p;
        }
    }
    return 0;  /* 表满 */
}

/* freeproc:释放一个进程(lab2 简化) */
void
freeproc(struct proc *p)
{
    p->state = UNUSED;
    p->trapframe = 0;
}
