/* ============ lab2 · syscall.h 系统调用号 ============
 * 约定:a7 = 系统调用号,a0~a2 = 参数,a0 = 返回值
 * 编号 1..22 与 user/usys.pl 生成的 22 个桩一一对应,连续无缺口
 * ======================================== */
#ifndef SYSCALL_H
#define SYSCALL_H

/* xv6 约定编号 */
#define SYS_fork    1
#define SYS_exit    2
#define SYS_wait    3
#define SYS_pipe    4
#define SYS_read    5
#define SYS_write   6
#define SYS_close   7
#define SYS_kill    8
#define SYS_exec    9
#define SYS_open    10
#define SYS_mknod   11
#define SYS_unlink  12
#define SYS_fstat   13
#define SYS_link    14
#define SYS_mkdir   15
#define SYS_chdir   16
#define SYS_dup    17
#define SYS_getpid  18
#define SYS_sbrk    19
#define SYS_pause   20
#define SYS_uptime  21
#define SYS_sync    22

/* 函数原型(汇编文件如 usys.S 不需要) */
#ifndef __ASSEMBLER__
void syscall(void);
#endif

#endif
