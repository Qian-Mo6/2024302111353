/* ============ lab2 阶段六 · fork/wait 自测版 main.c ============
 * 阶段五:测试 sys_exec(_user_test 调 exec("hi",0) -> 加载 hi 程序)
 * 阶段六:测试 sys_fork + sys_wait
 *         _user_test 调 fork,子进程 exec("hi"),父进程 wait 回收
 *
 * 流程:
 *   1. _user_test fork -> 切到子进程运行
 *   2. 子进程 a0=0,跳 child 分支,exec("hi") 加载 hi 程序
 *   3. hi 运行(getpid+printf+write),exit(0) -> 切回父进程
 *   4. 父进程 fork 返回子pid,wait 回收子进程,exit(0)
 *
 * 期望输出:
 *   hi: user program running, pid=2
 *   user process exited with 0      (子进程 hi exit)
 *   user process exited with 0      (父进程 exit)
 * ======================================== */
#include "types.h"
#include "riscv.h"
#include "proc.h"
#include "memlayout.h"

void consoleinit(void);
void plicinit(void);
void printf(char *fmt, ...);
void usertrapret(void);
struct proc *myproc(void);

extern char _user_test[];

pagetable_t kvminit(uint64 trampoline_pa, uint64 trapframe_pa, uint64 user_code_pa);
void kvminithart(pagetable_t kpgtbl);

extern char trampoline[];

/* 初始用户程序:exec("sh", 0) 启动最小 shell
 * sh 之后靠 fork/exec/wait/read/write 循环执行用户命令
 * 使用 PC 相对寻址(la 在 medany 模型下生成 auipc+addi)
 * 不需要栈 — exec 之前不调用任何需要栈的代码 */
__asm__(
    ".global _user_test\n"
    "_user_test:\n"
    "    # exec(\"sh\", 0) 加载并运行 sh\n"
    "    li a7, 9\n"            /* SYS_exec */
    "    la a0, name\n"         /* name 指向 "sh" */
    "    li a1, 0\n"            /* argv = 0,lab2 忽略 */
    "    ecall\n"
    "    # exec 不应返回;失败则 exit(1)\n"
    "    li a7, 2\n"            /* SYS_exit */
    "    li a0, 1\n"
    "    ecall\n"
    "    j .\n"
    "\n"
    "name:\n"
    "    .asciz \"sh\"\n"
);

void
main(void)
{
    consoleinit();
    printf("=== lab2 stage7: run sh ===\n");

    plicinit();
    printf("plic inited (UART0 IRQ=%d enabled)\n", UART0_IRQ);

    w_sie(r_sie() | SIE_SEIE);

    struct proc *p = allocproc();
    if (!p) { printf("allocproc failed\n"); while (1) {} }
    printf("proc pid=%d\n", p->pid);

    extern struct proc *curproc;
    curproc = p;

    /* 把内核 _user_test 所在页复制到 p->prog_page,作为初始用户程序
     * 使 VA 0x0 始终映射到进程自己的 prog_page,与 exec/fork 语义一致 */
    uint64 code_page = PGROUNDDOWN((uint64)_user_test);
    for (int i = 0; i < PGSIZE; i++)
        p->prog_page[i] = ((char *)code_page)[i];

    uint64 user_code_pa = (uint64)p->prog_page;
    uint64 trampoline_pa = PGROUNDDOWN((uint64)trampoline);
    uint64 trapframe_pa  = (uint64)p->trapframe;

    /* _user_test 入口 = 页内偏移(VA 0x0 起始)
     * sys_exec/sys_fork 会用 user_remap 重新映射 VA 0x0/0x1000 */
    p->trapframe->epc = (uint64)_user_test - code_page;
    /* sp=0x2000:栈顶,sys_exec 会映射 VA 0x1000 → 栈页 */
    p->trapframe->sp  = 0x2000;

    printf("user_epc=%x trampoline=%x tf=%x\n",
           p->trapframe->epc, trampoline_pa, trapframe_pa);

    pagetable_t kpgtbl = kvminit(trampoline_pa, trapframe_pa, user_code_pa);
    printf("kpgtbl=%x, switching satp...\n", (uint64)kpgtbl);

    kvminithart(kpgtbl);

    w_sstatus(r_sstatus() | SSTATUS_SUM);

    printf("satp OK, going to U mode...\n");
    printf("_user_test: exec(\"sh\", 0) -> shell\n");

    usertrapret();

    printf("ERROR: returned from usertrapret\n");
    while (1) {}
}
