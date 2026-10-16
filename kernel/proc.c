/* ============ lab2 · proc.c 进程管理 ============
 * 最小实现:全局 proc 表 + allocproc + myproc + fork + wait
 * lab2 单核单进程运行,curproc 全局指针
 *
 * fork 语义(lab2 简化):
 *   - 复制父进程 prog_page/stack_page 和 trapframe 到子进程
 *   - 子进程 trapframe->a0 = 0
 *   - user_remap 切换 VA 0x0/0x1000 到子进程页
 *   - curproc = 子进程,立即切换运行(无调度器)
 *   - 子进程 exit 时切回父进程
 * ======================================== */
#include "types.h"
#include "proc.h"
#include "riscv.h"
#include "memlayout.h"

/* lab2 简化:QEMU 需要 A/D 位(riscv.h 未定义) */
#define PTE_A (1L << 6)
#define PTE_D (1L << 7)

struct proc proc[NPROC];  /* 全局进程表 */
struct proc *curproc;     /* 当前运行进程(lab2 单核简化) */

/* trapframe 内存池:每进程独占一整页,page 对齐
 * 关键:TRAPFRAME VA 页内偏移为 0,user_remap 按页粒度映射,
 * 若多个 trapframe 挤在同一页内,重映射只会指向页首(错误的 trapframe) */
static char tf_pool[NPROC][PGSIZE] __attribute__((aligned(PGSIZE)));

/* user_remap:在全局内核页表中重新映射 VA(定义在 vm.c) */
void user_remap(uint64 va, uint64 pa, int perm);

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
            /* 分配 trapframe(独占一页) */
            p->trapframe = (struct trapframe *)tf_pool[p - proc];
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

/* fork:创建子进程
 * lab2 简化:立即切换到子进程运行,子进程 exit 后切回父进程
 * 内存:复制父 prog_page/stack_page 到子进程
 * 返回:父进程返回子 pid,子进程 trapframe->a0=0 */
struct proc *
fork(void)
{
    struct proc *parent = myproc();
    struct proc *child = allocproc();
    if (!child)
        return 0;

    /* 复制用户内存(程序页 + 栈页) */
    char *dst, *src;
    dst = child->prog_page;  src = parent->prog_page;
    for (int i = 0; i < PGSIZE; i++) dst[i] = src[i];
    dst = child->stack_page; src = parent->stack_page;
    for (int i = 0; i < PGSIZE; i++) dst[i] = src[i];

    /* 复制 trapframe,子进程 a0=0 (fork 返回 0) */
    *child->trapframe = *parent->trapframe;
    child->trapframe->a0 = 0;

    child->parent = parent;

    /* 切换到子进程:重新映射 VA 0x0/0x1000 到子进程页
     * 并把 TRAPFRAME 映射到子进程自己的 trapframe
     * (TRAPFRAME VA 全局唯一,satp 共用,切换进程必须重映射) */
    user_remap(0x0,    (uint64)child->prog_page,
               PTE_R | PTE_W | PTE_X | PTE_U | PTE_A | PTE_D);
    user_remap(0x1000, (uint64)child->stack_page,
               PTE_R | PTE_W | PTE_U | PTE_A | PTE_D);
    user_remap(TRAPFRAME, (uint64)child->trapframe, PTE_R | PTE_W);

    curproc = child;

    return child;  /* 返回子进程指针,sys_fork 取 child->pid 填父进程 a0 */
}

/* wait:等待子进程退出,回收资源
 * lab2 简化:fork 后立即切到子进程,子进程 exit 后切回父进程
 * 所以 wait 时子进程通常已 ZOMBIE */
int
wait(void)
{
    struct proc *p = myproc();

    for (struct proc *c = proc; c < &proc[NPROC]; c++) {
        if (c->parent == p && c->state == ZOMBIE) {
            int pid = c->pid;
            freeproc(c);
            return pid;
        }
    }
    return -1;  /* 没有 ZOMBIE 子进程 */
}

/* freeproc:释放一个进程(lab2 简化) */
void
freeproc(struct proc *p)
{
    p->state = UNUSED;
    p->trapframe = 0;
    p->parent = 0;
    p->pid = 0;
}
