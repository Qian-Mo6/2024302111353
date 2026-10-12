/* ============ lab2 · syscall.c 系统调用实现 ============
 * 由 usertrap() 调用,根据 trapframe->a7 分发到对应处理函数
 * 约定:a7=调用号,a0~a2=参数,返回值写入 trapframe->a0
 *
 * 阶段二实现:getpid / write / exit
 * 阶段四实现:read (从控制台缓冲区读)
 * 未实现的调用号统一返回 -1 (M3 要求:不死机)
 * ======================================== */
#include "types.h"
#include "proc.h"
#include "syscall.h"

void consputc(char c);
int  consoleread(char *user_dst, int n);
void printf(char *fmt, ...);

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
 * lab2 简化:标记 ZOMBIE 后停机(lab4 改走调度器) */
static uint64
sys_exit(void)
{
    struct proc *p = myproc();
    int status = p->trapframe->a0;

    printf("user process exited with %d\n", status);
    freeproc(p);

    for (;;) { }
    return 0;
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
    default:
        p->trapframe->a0 = -1;
        break;
    }
}
