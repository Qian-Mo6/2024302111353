/* ============ lab2 · syscall.c 系统调用实现 ============
 * 由 usertrap() 调用,根据 trapframe->a7 分发到对应处理函数
 * 约定:a7=调用号,a0~a2=参数,返回值写入 trapframe->a0
 *
 * 阶段二实现:getpid / write / exit
 * 阶段四实现:read (从控制台缓冲区读)
 * 阶段五实现:exec (从内嵌程序表 _uprog_table 加载程序)
 * 阶段六实现:fork / wait (进程创建与回收)
 * 未实现的调用号统一返回 -1 (M3 要求:不死机)
 * ======================================== */
#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "proc.h"
#include "syscall.h"

/* lab2 简化:QEMU 需要 A/D 位(riscv.h 未定义,与 vm.c 保持一致) */
#define PTE_A (1L << 6)
#define PTE_D (1L << 7)

void consputc(char c);
int  consoleread(char *user_dst, int n);
void printf(char *fmt, ...);

/* user_remap:在全局内核页表中重新映射 VA(定义在 vm.c) */
void user_remap(uint64 va, uint64 pa, int perm);

/* sys_getpid:返回当前进程 pid */
static uint64
sys_getpid(void)
{
    return myproc()->pid;
}

/* sys_write(fd, buf, len):将用户缓冲区写入文件描述符
 * lab2 只支持 fd=1 (stdout)
 * 简化:内核和用户共用页表,可直接访问用户地址(lab3 改 copyin) */
static uint64
sys_write(void)
{
    struct proc *p = myproc();
    int fd = p->trapframe->a0;
    char *buf = (char *)p->trapframe->a1;
    int len = p->trapframe->a2;

    if (fd != 1)
        return -1;

    for (int i = 0; i < len; i++)
        consputc(buf[i]);

    return len;
}

/* sys_read(fd, buf, n):从文件描述符读 n 字节到用户缓冲区
 * lab2 只支持 fd=0 (stdin) → consoleread
 * 简化:内核和用户共用页表,可直接写入用户地址(lab3 改 copyout) */
static uint64
sys_read(void)
{
    struct proc *p = myproc();
    int fd = p->trapframe->a0;
    char *buf = (char *)p->trapframe->a1;
    int n = p->trapframe->a2;

    if (fd != 0)
        return -1;

    return consoleread(buf, n);
}

/* sys_exit(status):退出当前进程
 * lab2 简化:标记 ZOMBIE 后切回父进程(若有),否则停机
 * Stage6: 切回父进程时需恢复父进程的 VA 0x0/0x1000 映射 */
static uint64
sys_exit(void)
{
    struct proc *p = myproc();
    int status = p->trapframe->a0;

    printf("user process exited with %d\n", status);

    /* 标记 ZOMBIE (wait 时 freeproc) */
    p->state = ZOMBIE;

    /* 切回父进程:恢复父进程的 VA 映射,并把 TRAPFRAME 映射回父进程 */
    if (p->parent) {
        user_remap(0x0,    (uint64)p->parent->prog_page,
                   PTE_R | PTE_W | PTE_X | PTE_U | PTE_A | PTE_D);
        user_remap(0x1000, (uint64)p->parent->stack_page,
                   PTE_R | PTE_W | PTE_U | PTE_A | PTE_D);
        user_remap(TRAPFRAME, (uint64)p->parent->trapframe, PTE_R | PTE_W);
        extern struct proc *curproc;
        curproc = p->parent;
    } else {
        /* 初始进程 exit:无父进程,停机(lab4 改走调度器) */
        for (;;) { }
    }
    return 0;  /* 不会返回(curproc 已切到父进程) */
}

/* sys_exec(name, argv):从内嵌程序表 _uprog_table 加载程序
 *   a0 = name 指针(lab2 内核/用户共用页表,可直接读用户地址)
 *   a1 = argv (lab2 忽略,规格允许)
 *
 * _uprog_table 布局(userimg.S 生成):
 *   每项 = {u64 start; u64 end; char name[]; .balign 8; 二进制}
 *   以 {0, 0} 结尾
 *
 * Stage6: 程序复制到当前进程的 prog_page(非全局),支持多进程独立 */
static uint64
sys_exec(void)
{
    struct proc *p = myproc();
    char *name = (char *)p->trapframe->a0;

    /* argv 暂忽略(lab2 规格) */

    /* sh 传入的一整行含结尾 '\n'(gets 不剥离),截到首个空白/换行 */
    char nbuf[64];
    int nl = 0;
    for (; nl < 63; nl++) {
        char c = name[nl];
        if (c == '\0' || c == '\n' || c == '\r' || c == ' ')
            break;
        nbuf[nl] = c;
    }
    nbuf[nl] = '\0';

    extern char _uprog_table[];
    uint64 cur = (uint64)_uprog_table;

    while (1) {
        uint64 start = *(uint64 *)cur;
        uint64 end   = *(uint64 *)(cur + 8);

        /* 终止哨兵 {0,0} */
        if (start == 0 && end == 0)
            break;

        char *prog_name = (char *)(cur + 16);

        /* 逐字符比对 nbuf == prog_name */
        int match = 1;
        for (int i = 0; ; i++) {
            if (nbuf[i] != prog_name[i]) { match = 0; break; }
            if (nbuf[i] == '\0') break;
        }

        if (match) {
            uint64 sz = end - start;
            if (sz > PGSIZE) return -1;

            /* 复制程序二进制到当前进程的 prog_page,剩余清零 */
            char *src = (char *)start;
            char *dst = p->prog_page;
            for (uint64 i = 0; i < sz; i++)
                dst[i] = src[i];
            for (uint64 i = sz; i < PGSIZE; i++)
                dst[i] = 0;

            /* 重新映射用户页:程序页(需 W 供写 BSS/数据) + 栈页 */
            user_remap(0x0,    (uint64)p->prog_page,
                       PTE_R | PTE_W | PTE_X | PTE_U | PTE_A | PTE_D);
            user_remap(0x1000, (uint64)p->stack_page,
                       PTE_R | PTE_W | PTE_U | PTE_A | PTE_D);

            /* 设置新程序入口和栈顶 */
            p->trapframe->epc = 0;
            p->trapframe->sp  = 0x2000;

            return 0;  /* 成功 */
        }

        /* 下一项表头在 binary 之后(end 处),.balign 8 后 */
        cur = (end + 7) & ~7UL;
    }

    return -1;  /* 未找到程序 */
}

/* sys_fork:创建子进程(调用 proc.c 的 fork)
 * fork 内部会切换 curproc 到子进程
 * 返回子 pid 给父进程 trapframe->a0(由 syscall() 设置) */
static uint64
sys_fork(void)
{
    struct proc *child = fork();
    return child ? child->pid : -1;
}

/* sys_wait(status):等待子进程退出,回收资源
 * lab2 简化:忽略 status 指针,只返回子进程 pid */
static uint64
sys_wait(void)
{
    return wait();
}

/* syscall:由 usertrap 调用,根据 a7 分发
 * 未实现的调用号走 default 返回 -1 (M3) */
void
syscall(void)
{
    struct proc *p = myproc();
    uint64 num = p->trapframe->a7;

    switch (num) {
    case SYS_getpid:
        p->trapframe->a0 = sys_getpid();
        break;
    case SYS_write:
        p->trapframe->a0 = sys_write();
        break;
    case SYS_read:
        p->trapframe->a0 = sys_read();
        break;
    case SYS_exit:
        p->trapframe->a0 = sys_exit();
        break;
    case SYS_exec:
        p->trapframe->a0 = sys_exec();
        break;
    case SYS_fork:
        p->trapframe->a0 = sys_fork();
        break;
    case SYS_wait:
        p->trapframe->a0 = sys_wait();
        break;
    default:
        p->trapframe->a0 = -1;
        break;
    }
}
