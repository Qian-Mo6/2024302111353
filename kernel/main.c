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
#include "course_sid.h"   /* 学号与个性化参数(COURSE_SID / LAB2_BUF_SIZE 等) */
#include "vm.h"           /* lab3:kinit/kalloc/kfree 接口 */

/* lab3 阶段一临时验证开关:1=上电自检物理页分配器。
 * 已验证 PASS(对齐/升序/清零/守恒/触顶 32107 次后返回 0),正式构建置 0。 */
#define LAB3_KALLOC_SELFTEST 0

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

    /* lab1 回归:启动首行打印 banner(协议 0:明文 + 换行)
     * 内容 = 学号十进制 + (学号 % 97) 的小写十六进制,如 "2024302111353 0x2e" */
    printf("%d %x\n", (long)COURSE_SID, (uint64)(COURSE_SID % 97));

    /* lab3 阶段一:物理页分配器初始化(位图,升序,配额=总页数-LAB3_MEMCAP_DELTA) */
    kinit();

#if LAB3_KALLOC_SELFTEST
    /* 阶段一临时验证(验证通过后把上面的宏置 0):
     *   1) 分配 3 页:4KB 对齐、严格升序、内容已清零;
     *   2) 释放中间页再分配:升序语义下应重新拿到该最低空闲页;
     *   3) 计数守恒:全部释放后 used 回到基线;
     *   4) 耗尽:持续分配恰好在 cap 次后返回 0,且内核不 panic;
     *      随后全部释放,used 再次守恒。 */
    {
        int ok = 1;
        uint64 baseline = kpages_used();

        char *p1 = kalloc(), *p2 = kalloc(), *p3 = kalloc();
        if (!p1 || !p2 || !p3) ok = 0;
        if (ok && (((uint64)p1 & 0xFFF) || ((uint64)p2 & 0xFFF) || ((uint64)p3 & 0xFFF)))
            ok = 0;                                  /* 对齐 */
        if (ok && !(p1 < p2 && p2 < p3))
            ok = 0;                                  /* 升序 */
        if (ok)
            for (int i = 0; i < PGSIZE; i++)
                if (p1[i] || p2[i] || p3[i]) { ok = 0; break; }   /* 清零 */

        kfree(p2);
        char *p4 = kalloc();
        if (p4 != p2) ok = 0;                        /* 升序:重发最低空闲页 */

        kfree(p1); kfree(p3); kfree(p4);
        if (kpages_used() != baseline) ok = 0;       /* 守恒 */

        static void *hold[(PHYSTOP - KERNBASE) / PGSIZE];
        uint64 n = 0;
        void *q;
        while ((q = kalloc()) != 0)
            hold[n++] = q;
        if (n != kpages_cap()) ok = 0;               /* 恰好触顶 */
        if (kalloc() != 0) ok = 0;                   /* 触顶后仍返回 0 */
        while (n > 0)
            kfree(hold[--n]);
        if (kpages_used() != baseline) ok = 0;       /* 再次守恒 */

        printf("kalloc selftest: %s (cap=%d)\n", ok ? "PASS" : "FAIL", kpages_cap());
        if (!ok) { intr_off(); while (1) {} }
    }
#endif

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
