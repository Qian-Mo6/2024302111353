/* ============ lab2 · syscall.c 系统调用实现 ============
 * 由 usertrap() 调用,根据 trapframe->a7 分发到对应处理函数
 * 约定:a7=调用号,a0~a2=参数,返回值写入 trapframe->a0
 *
 * lab3 阶段四改造:
 *   - 用户指针一律经 copyin/copyout/copyinstr 受控拷贝,
 *     内核不再直接解引用用户地址(独立用户页表后直址即缺页);
 *   - sys_exec 构建全新地址空间(uvmcreate+uvmload),全部映射成功后
 *     才 uvmfree 旧空间;任一失败保留旧空间,进程可继续运行;
 *   - sys_wait 回写子进程退出状态;sys_exit 与异常被杀共用 exit_current。
 * ======================================== */
#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "proc.h"
#include "syscall.h"
#include "vm.h"

/* QEMU 需要 A/D 位(riscv.h 未定义,与 vm.c 保持一致) */
#define PTE_A (1L << 6)
#define PTE_D (1L << 7)

void consputc(char c);
int  consoleread(char *kbuf, int n);   /* 读入内核缓冲区,由 sys_read copyout */
void printf(char *fmt, ...);

/* sys_getpid:返回当前进程 pid */
static uint64
sys_getpid(void)
{
    return myproc()->pid;
}

/* sys_write(fd, buf, len):将用户缓冲区写入文件描述符
 * 只支持 fd=1 (stdout)。用户缓冲区经 copyin 分块拷入内核后输出。 */
static uint64
sys_write(void)
{
    struct proc *p = myproc();
    int fd = p->trapframe->a0;
    uint64 buf = p->trapframe->a1;
    int len = (int)p->trapframe->a2;

    if (fd != 1 || len < 0)
        return -1;

    char kbuf[128];
    int off = 0;
    while (off < len) {
        int n = len - off;
        if (n > (int)sizeof(kbuf)) n = (int)sizeof(kbuf);
        if (copyin(p->pagetable, kbuf, buf + off, (uint64)n) < 0)
            return -1;
        for (int i = 0; i < n; i++)
            consputc(kbuf[i]);
        off += n;
    }
    return len;
}

/* sys_read(fd, buf, n):从文件描述符读 n 字节到用户缓冲区
 * 只支持 fd=0 (stdin):consoleread 读入内核缓冲区,再 copyout 给用户。
 * read 允许短读:单次最多交付 sizeof(kbuf) 字节。 */
static uint64
sys_read(void)
{
    struct proc *p = myproc();
    int fd = p->trapframe->a0;
    uint64 buf = p->trapframe->a1;
    int n = (int)p->trapframe->a2;

    if (fd != 0 || n < 0)
        return -1;

    char kbuf[64];
    int m = n > (int)sizeof(kbuf) ? (int)sizeof(kbuf) : n;
    int r = consoleread(kbuf, m);
    if (r > 0 && copyout(p->pagetable, buf, kbuf, (uint64)r) < 0)
        return -1;
    return r;
}

/* sys_exit(status):退出当前进程
 * 标 ZOMBIE 后切回父进程(若有),状态留给 wait 回写 */
static uint64
sys_exit(void)
{
    struct proc *p = myproc();
    int status = (int)p->trapframe->a0;

    printf("user process exited with %d\n", status);
    exit_current(status);
    return 0;  /* 不会返回(curproc 已切到父进程) */
}

/* sys_exec(name, argv):从内嵌程序表 _uprog_table 加载程序
 *   a0 = name 用户指针(copyinstr 受控拷入)
 *   a1 = argv (忽略,规格允许)
 *
 * _uprog_table 布局(userimg.S 生成,ABI 勿改):
 *   每项 = {u64 start; u64 end; char name[]; .balign 8; 二进制}
 *   以 {0, 0} 结尾
 * _uprog_meta(构建期从 ELF 提取,与表项同序):
 *   每项 = {u64 wstart; u64 memsz} —— .data 起始 VA(页对齐)与 end 符号,
 *   用于区分代码/只读页(RXU,无 W)与数据/bss 页(RWU)。
 *
 * 新地址空间全部构建成功后才释放旧空间;任一失败保留旧空间返回 -1。 */
static uint64
sys_exec(void)
{
    struct proc *p = myproc();

    /* argv 暂忽略(lab2 规格) */

    /* 受控拷入程序名,并截到首个空白/换行(sh 传入的一整行含 '\n') */
    char nbuf[64];
    if (copyinstr(p->pagetable, nbuf, p->trapframe->a0, sizeof(nbuf)) < 0)
        return -1;
    for (int i = 0; i < (int)sizeof(nbuf); i++) {
        char c = nbuf[i];
        if (c == '\n' || c == '\r' || c == ' ') { nbuf[i] = '\0'; break; }
        if (c == '\0') break;
    }

    extern char _uprog_table[];
    extern uint64 _uprog_meta[];
    uint64 cur = (uint64)_uprog_table;
    int idx = 0;   /* 表项序号,与 _uprog_meta 同序 */

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
            uint64 wstart = _uprog_meta[idx * 2];
            uint64 memsz  = _uprog_meta[idx * 2 + 1];

            /* 构建全新地址空间:新页表 + TRAPFRAME + 代码/数据/保护页/栈 */
            pagetable_t npt = uvmcreate();
            if (!npt)
                return -1;
            if (mappages(npt, TRAPFRAME, PGSIZE, (uint64)p->trapframe,
                         PTE_R | PTE_W | PTE_A | PTE_D) != 0) {
                uvmfree(npt, 0);
                return -1;
            }
            uint64 nsz = 0, nsp = 0;
            if (uvmload(npt, (char *)start, end - start,
                        wstart, memsz, &nsz, &nsp) != 0) {
                uvmfree(npt, 0);    /* 旧空间原样保留,进程可继续运行 */
                return -1;
            }

            /* 全部成功:释放旧空间,切换到新空间 */
            uvmfree(p->pagetable, p->sz);
            p->pagetable = npt;
            p->sz = nsz;

            /* 新程序入口与栈顶 */
            p->trapframe->epc = 0;
            p->trapframe->sp  = nsp;

            return 0;  /* 成功 */
        }

        /* 下一项表头在 binary 之后(end 处),.balign 8 后 */
        cur = (end + 7) & ~7UL;
        idx++;
    }

    return -1;  /* 未找到程序 */
}

/* sys_sbrk(n, strategy):地址空间动态增长(EAGER)
 *   a0 = 增减字节数(有符号),a1 = 策略(SBRK_EAGER/SBRK_LAZY)
 * 语义:
 *   n > 0:EAGER 立即为新增区间分配物理页并建立 RWU 映射;
 *   n < 0:收缩边界并回收整页;
 *   n = 0:仅查询当前边界。
 * 返回增长前的边界(即新分配区首地址,用户库与官方测试按此使用);
 * 失败返回 -1:uvmalloc 已把本次分配完整回滚,配额耗尽或越过
 * TRAPFRAME 均只报错,内核不 panic。
 * SBRK_LAZY 在 lab3 按 EAGER 执行(缺页按需分配留待 lab5)。 */
static uint64
sys_sbrk(void)
{
    struct proc *p = myproc();
    long n = (long)p->trapframe->a0;
    uint64 strategy = p->trapframe->a1;

    (void)strategy;                  /* lab3:LAZY 请求同样按 EAGER 分配 */

    uint64 oldsz = p->sz;
    if (n > 0) {
        uint64 grow = (uint64)n;
        if (oldsz + grow < oldsz)                    /* 加法回绕 */
            return -1;
        if (uvmalloc(p->pagetable, oldsz, oldsz + grow) != 0)
            return -1;
        p->sz = oldsz + grow;
    } else if (n < 0) {
        uint64 shrink = (uint64)0 - (uint64)n;       /* 避免 -LONG_MIN 溢出 */
        if (shrink > oldsz)                          /* 收缩越过地址空间起点 */
            return -1;
        uvmdealloc(p->pagetable, oldsz, oldsz - shrink);
        p->sz = oldsz - shrink;
    }
    return oldsz;
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
 * status 用户指针非 0 时回写子进程退出状态 */
static uint64
sys_wait(void)
{
    return wait(myproc()->trapframe->a0);
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
    case SYS_sbrk:
        p->trapframe->a0 = sys_sbrk();
        break;
    default:
        p->trapframe->a0 = -1;
        break;
    }
}
