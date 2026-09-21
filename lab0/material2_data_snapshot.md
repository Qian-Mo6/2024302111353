# 材料二 · 核心数据结构全景快照（exec 完成时刻）

> lab0 实验交付物。捕捉 kexec 刚返回、userret/sret 即将执行、echo main 首条指令未运行的截面。
> 配套图：`material2_data_snapshot.svg`。

## 截面时刻定义

- **进程视角**：echo 进程 state=RUNNABLE，trapframe.epc=elf.entry，trapframe.a0/a1=argc/argv
- **页表视角**：echo.pagetable 已被 kexec 替换为新页表，旧页表已 freepagetable 释放
- **文件视角**：echo.ofile 是 fork 时从 shell 复制的指针副本，与 shell 共享同一份 struct file

## 快照 ① 进程表（proc[NPROC]）

### 全局数组
- `proc[NPROC]` —— NPROC=64，仅列活跃 slot

### 三个活进程字段对照

| 字段 | init (pid=1) | shell (pid=2) | echo (pid=3) ★主角 |
|---|---|---|---|
| state | SLEEPING | SLEEPING | RUNNABLE |
| sleep 原因 | wait shell 退出 | wait echo 退出 | （未睡，等下次调度） |
| parent | 自身/initproc | &init | &shell |
| pagetable | init 页表 | shell 页表 | ★ kexec 刚建的新页表 |
| sz | init ELF 大小 | shell ELF 大小 | echo ELF sz + (USERSTACK+1)*PGSIZE |
| trapframe | &p[0].tf（init 的现场） | &p[1].tf | &p[2].tf，epc=elf.entry, sp=栈顶, a0=argc, a1=argv |
| kstack | TRAMPOLINE-2*PGSIZE | TRAMPOLINE-4*PGSIZE | TRAMPOLINE-6*PGSIZE |
| ofile[0] stdin | → file_A | → file_A | → file_A |
| ofile[1] stdout | → file_B | → file_B | → file_B |
| ofile[2] stderr | → file_C | → file_C | → file_C |

### 关键观察
1. **echo.pagetable 已替换**：kexec 用 `proc_pagetable(p)` 建新页表 + `uvmalloc` 装代码段 + `loadseg` 复制 ELF + `uvmalloc` 装栈，全部成功后才 `proc_freepagetable(p->pagetable, p->sz)` 释放旧页表
2. **echo.ofile 与 shell 共享同一份 file_B**：fork 时 `ofilecopy` 逐个复制 ofile 指针，每个 fd 对应的 file.ref++ —— exec 不动 ofile 表
3. **echo.trapframe 重新填好**：kexec 末尾 `p->trapframe->epc = elf.entry; p->trapframe->sp = sp; p->trapframe->a0 = argc; p->trapframe->a1 = argv` —— sret 后 PC 跳到 echo main 第一行
4. **state 不是 RUNNING 而是 RUNNABLE**：kexec 在 sys_exec 里跑，return 后 syscall 返回路径走 userret→sret 回用户态。此时仍在内核态，等下次时钟中断/调度切回 echo 才真正 RUN echo 第一条指令

## 快照 ② echo 进程页表（Sv39 三级）

### Sv39 虚拟地址布局（从高到低）

| 虚拟地址 | 段名 | PTE 权限 | 物理页来源 |
|---|---|---|---|
| 0x3fffffe000 (TRAMPOLINE) | uservec/userret 跳板 | R-X-U | ★ 全局共享物理页（trampoline.S 编译产出） |
| 0x3fffffd000 (TRAPFRAME) | 40 寄存器现场 | R-W-U | 进程私有（kexec 时 proc_pagetable 重新映射到 p->trapframe） |
| sz-1 ~ sz-USERSTACK*PGSIZE | 用户栈 | R-W-U | 进程私有（uvmalloc 时 kalloc） |
| sz-(USERSTACK+1)*PGSIZE | guard page | V=0 (无效) | 故意无效，防栈溢出（uvmclear 清 V 位） |
| sz 之下到 data 之上 | 堆区 | R-W-U | 进程私有（lab0 时刻为空，sbrk 扩展） |
| data 起 ~ data 末 | 数据段 | R-W-U | 进程私有（uvmalloc + loadseg 装入） |
| 0 ~ text 末 | 代码段 | R-X-U | 进程私有（uvmalloc + loadseg 装入） |

**地址计算**：
- MAXVA = 1L << 38 = 0x4000000000（SV39 最大 VA）
- TRAMPOLINE = MAXVA - PGSIZE = 0x3fffffe000
- TRAPFRAME = TRAMPOLINE - PGSIZE = 0x3fffffd000
- KSTACK(p) = TRAMPOLINE - (p+1) * 2 * PGSIZE（内核栈，进程私有）
- sz = PGROUNDUP(ELF sz) + (USERSTACK+1) * PGSIZE

### Sv39 三级页表结构
- L2 root pagetable：512 个 PTE，9 位 VA 索引 [38:30]
- L1 pagetable：512 个 PTE，9 位 VA 索引 [29:21]
- L0 pagetable：512 个 PTE，9 位 VA 索引 [20:12]
- 12 位页内偏移 [11:0]

### 权限位速记
- V = 有效（映射存在）
- R = 可读
- W = 可写
- X = 可执行
- U = 用户态可访问

### 关键观察
1. **TRAMPOLINE 物理页全局共享**：所有进程的页表都把 TRAMPOLINE 虚拟地址映射到同一份物理页（trampoline.S 代码）。uvmcopy 时 PTE 只复制（指向同一物理页），不复制物理页本身。
2. **TRAPFRAME 是进程私有**：但 kexec 时 TRAPFRAME 物理页不释放——`proc_pagetable` 建新页表时通过 `mappages(new, TRAPFRAME, PGSIZE, p->trapframe, PTE_R|PTE_W|PTE_U)` 把同一份物理页重新映射到新页表
3. **代码段 PTE_R|PTE_X|PTE_U 无 W 位**：代码段不可写，写就 page fault（材料三会涉及 scause=15 的 store page fault）
4. **guard page 是 uvmclear 的产物**：exec 末尾 `uvmclear(pagetable, sz-(USERSTACK+1)*PGSIZE)` 把这一页的 PTE V 位清掉——任何越界写栈都立即 page fault

## 快照 ③ 文件表引用链

### ofile → file → inode 链路

```
init.ofile[1]  ─┐
shell.ofile[1] ─┼─→ struct file_B {
echo.ofile[1]  ─┘    ref = 3,              ← 三个进程共享同一份
                     type = FD_DEVICE,    ← 设备文件，走 devsw 分发
                     readable = 0,
                     writable = 1,
                     ip = &console_inode, ← 指向 inode
                     off = 0
                   }                      ──→ struct console_inode {
                                            ref = N,
                                            valid = 1,
                                            type = I_DEVICE,  ← 设备节点
                                            major = CONSOLE = 1,
                                            minor = 0,
                                            nlink = ...,
                                            (无 size, 设备文件)
                                          }
```

### write(1, "hi\n") 调用栈对照

| 层 | 文件:函数 | 关键操作 |
|---|---|---|
| 用户 | user/echo.c: main | write(1, "hi\n", 3) |
| 桩 | user/usys.pl | li a7, SYS_write; ecall |
| 系统调用入口 | kernel/sysfile.c: sys_write | argfd(1, &fd, &f) 从 p->ofile[fd] 取出 file_B |
| 通用文件层 | kernel/file.c: filewrite | switch(f->type) case FD_DEVICE: if(f->writable) devsw[f->ip->major].write(...) |
| 设备分发 | kernel/devsw[] 表 | devsw[CONSOLE=1].write = consolewrite |
| 控制台驱动 | kernel/console.c: consolewrite | 持 cons.lock，逐字符入 cons.buf |
| UART 驱动 | kernel/uart.c: uartputc_sync | 写 UART THR 寄存器（MMIO 0x10000000） |
| 硬件 | QEMU virt 16550 | THR 寄存器值 → 终端打印 |

### 关键观察
1. **file_B.ref = 3**：fork 时 ofilecopy 把指针逐个复制，每复制一个 fd 对应的 file.ref++。exec 不动 ofile 表，所以 ref 保持 3
2. **console inode 是 I_DEVICE**：不走磁盘 fs 路径，没有 size 字段，只有 major/minor 标识设备
3. **devsw[1] = consolewrite**：major=CONSOLE=1，devsw 是全局函数指针表，在 consoleinit 里注册
4. **写 fd 1 不经过磁盘**：与写普通文件（FD_INODE）路径不同——普通文件走 filewrite → writei → buf/bcache → virtio 驱动 → 磁盘；console 走 filewrite → devsw[major].write → consolewrite → uartputc

---

## ★ 自主思考批注

### 批注 1：exec 重建用户页表，为什么不会清空进程的 ofile 数组？xv6 原生是否实现 FD_CLOEXEC？
**位置**：快照 ① echo.ofile、快照 ③ 文件表引用链

AI：ofile 数组存于 `struct proc`（内核数据结构，不在用户地址空间），exec 只删除用户侧页表，不会触碰 proc 内的 ofile，因此默认继承所有文件描述符。xv6 原版**没有实现 FD_CLOEXEC**；Linux 下该标记会在 exec 时自动关闭对应 fd、递减 file 引用计数。

### 批注 2：exec 是复用 fork 得到的旧 trapframe 物理页，还是分配新物理页、仅保留固定虚拟地址？
**位置**：快照 ② 页表 - TRAPFRAME

AI：exec **复用同一个 `p->trapframe` 物理页** ，只重建用户页表。 `proc_pagetable(p)` 建新页表时通过 `mappages(new, TRAPFRAME, PGSIZE, p->trapframe, PTE_R|PTE_W)` 把同一份物理页重新映射到固定虚拟地址 `0x3fffffd000` ；旧的 trapframe 物理页 **不释放** （ `proc_freepagetable` 调 `uvmunmap(..., 0)` 第 4 参=0 表示 do _free=0，只解除映射不释放物理页）。exec 末尾写入 `p->trapframe->epc = elf.entry; p->trapframe->sp = sp; p->trapframe->a0/a1 = argc/argv`，sret 后 CPU 跳到 echo main 第一行。

### 批注 3：trampoline 是内核跳板代码，为什么 PTE 的 U 位要打开？用户态直接跳转过去执行会怎样？
**位置**：快照 ② 页表 - TRAMPOLINE

AI：trampoline 是 uservec/userret 跳板代码，PTE 权限是 `PTE_ R|PTE _X`（无 W、无 U）。源码注释明确："only the supervisor uses it, on the way to/from user space, so not PTE_ U"。 为什么不需要 PTE _U？因为 ecall/sret **硬件** 完成特权级切换后 CPU 才跳到 trampoline：ecall 一执行，硬件立刻把 CPU 切到 S 模式，PC 跳到 stvec 指向的 uservec——此刻 CPU 已经在 S 模式，访问 trampoline 不需要 U 位。userret 末尾 sret 也是同理：sret 执行时 CPU 仍 S 模式，sret 完成切到 U 模式那一刻 PC 已经跳出 trampoline 到用户代码段。所以 trampoline 全程在 S 模式被访问，U 位关闭是安全且正确的。

### 批注 4：guard page 是无效映射页，它的作用是什么？xv6 是否会自动扩展用户栈？
**位置**：快照 ② 页表 guard page

AI：guard page 放置在用户栈下方、V 位清零（无映射），作为栈溢出的"撞墙检测"。xv6 **不会自动扩展用户栈**；一旦栈向下越界访问 guard page，触发缺页异常，进程直接崩溃。sbrk 也不能越过 guard 页分配内存。

---

### 我的额外批注（对比/分类总结）

#### 批注 5：fork 复制 fd 指针 VS exec 保持 fd VS FD_CLOEXEC

1. **fork**：遍历父进程 `ofile[]`，复制每个 file 指针，**每复制一条，f->ref++**；父子共享同一个 struct file。
2. **exec**：**完全不修改 ofile 数组、不修改 file.ref**。struct proc 的 ofile 不属于用户地址空间，kexec 只删除用户侧内存页表，不会触碰 proc 里的 ofile。
3. **FD_CLOEXEC**：xv6 原版**未实现**；在真实 Linux 中，带这个标记的 fd，exec 时自动 close，ref 减一。

总结：fork 共享 fd；exec 默认继承全部 fd；close-on-exec 是 exec 时的自动清理开关，xv6 缺失该特性。

#### 批注 6：trampoline（全局固定物理页） VS trapframe（每进程私有物理页）

- **trampoline** ： **全局唯一物理页** ，所有进程页表映射到同一个 VA（TRAMPOLINE=0x3fffffe000），权限 R-X（无 W、无 U）。所有进程的 trampoline PTE 都指向同一物理页；exec 不更换。 
- **trapframe** ： **每个进程独立物理页** ，权限 R-W（无 X、无 U）。 `p->trapframe` 在 allocproc 时 kalloc 一次，进程整个生命周期内指针不变；fork 时 copyproc 调 `proc_pagetable` 在子进程新页表里映射子进程自己的 trapframe 物理页；exec 时 `proc_pagetable` 在新页表里 **重新映射同一份 p->trapframe 物理页** 到 TRAPFRAME VA，旧物理页不释放。

总结：VA 都是固定高地址；trampoline 物理页全局一份；trapframe 物理页进程私有，allocproc 时分配、freeproc 时释放，exec/fork 不动它。

#### 批注 7：trampoline 的 U 位 = 允许 CPU 在 U 特权级取指 ≠ 用户态代码可以随意跳转执行

- **PTE_U 含义** ：U=1 表示 U 模式可访问；U=0 表示只有 S 模式可访问。trampoline 的 U=0 是有意为之。
- **为什么 trampoline 可以 U=0** ：ecall/sret 都是硬件指令，执行瞬间 CPU 已切到 S 模式才跳 trampoline。uservec（保存用户现场、切栈）和 userret（恢复用户现场、sret）全在 S 模式跑，不需要 U 位。
- **U=0 的安全收益** ：用户态恶意代码无法直接 `jal` 跳到 trampoline 取指——任何 U 模式对 TRAMPOLINE 地址的访问都会 page fault（scause=15）。这把 trampoline 隔离成"硬件切换专用跳板"，杜绝用户态乱跳。

总结：U=0 不是疏漏，是刻意的安全边界——trampoline 只在硬件 trap 路径上的 S 模式执行，用户态根本不该碰它。

#### 批注 8：`struct file.ref`（文件引用计数） VS `struct inode.ref`（inode 引用计数）

- **`file.ref`**：**进程打开句柄计数**。每次 fork 复制 fd 指针、每次 dup，ref++；每次 close、exit 回收 fd，ref--；ref==0 释放 struct file。多个进程可以共享同一个 file，共享文件偏移 off。
- **`inode.ref`**：**磁盘 inode 的内存缓存引用计数**。只要内存里还有任何 file 引用这个 inode，inode 就不会被释放；多个不同 file 可以指向同一个 inode（硬链接、多次 open 同一个文件）。inode.ref 和 file.ref 两套计数互相独立。

总结：file.ref 管"打开的句柄"和文件偏移；inode.ref 管磁盘文件元数据缓存。console 设备 inode ref 固定，只要内核启动就常驻内存。

---

## 验收检查清单对照

- [✓] 进程表快照：至少列出 init/shell/echo 三进程的 state/parent/pagetable/sz/ofile/trapframe 字段
- [✓] 页表快照：完整画出 echo 进程虚拟地址空间的 trampoline/trapframe/代码段/数据段/堆区/guard/用户栈，标注虚拟地址区间和 PTE 权限位
- [✓] 文件表快照：ofile → file → inode 引用链，标注 ref/type/major 字段
- [✓] 包含至少 3 处自主思考批注（实际 8 条：4 条核心批注 + 4 条对比/分类总结）
- [✓] 截面时刻定义清楚（kexec 返回、sret 未执行）
- [✓] 图纸已保存到 `~/oslab/2024302111353-kernel/lab0/material2_data_snapshot.svg`

## 文件路径

- 图：`~/oslab/2024302111353-kernel/lab0/material2_data_snapshot.svg`
- 本说明：`~/oslab/2024302111353-kernel/lab0/material2_data_snapshot.md`
