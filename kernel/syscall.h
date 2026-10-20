/* ============ 系统调用号(lab3 增量包统一定义) ============
 * 约定:a7 = 系统调用号,a0~a2 = 参数,a0 = 返回值
 * 编号 1..22 与 user/usys.pl 生成的 22 个桩一一对应,连续无缺口;
 * 本表以 gifts/kernel/syscall.h 为准(SYS_sbrk = 12)。
 * 注意:main.c 的引导代码内嵌了 SYS_exec 立即数,改动本表须同步。
 * ======================================== */
#ifndef SYSCALL_H
#define SYSCALL_H

#define SYS_fork    1
#define SYS_exit    2
#define SYS_wait    3
#define SYS_pipe    4
#define SYS_read    5
#define SYS_kill    6
#define SYS_exec    7
#define SYS_fstat   8
#define SYS_chdir   9
#define SYS_dup     10
#define SYS_getpid  11
#define SYS_sbrk    12
#define SYS_pause   13
#define SYS_uptime  14
#define SYS_open    15
#define SYS_write   16
#define SYS_mknod   17
#define SYS_unlink  18
#define SYS_link    19
#define SYS_mkdir   20
#define SYS_close   21
#define SYS_sync    22

/* 函数原型(汇编文件如 usys.S 不需要) */
#ifndef __ASSEMBLER__
void syscall(void);
#endif

#endif
