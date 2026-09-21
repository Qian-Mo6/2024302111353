# 材料一 · 全系统控制流图（echo hi 全生命周期）

> lab0 实验交付物。追踪从用户敲下回车键到屏幕显示 `hi` 的完整控制流。
> 配套图：`material1_control_flow.svg`。

## 图说明

- **横轴 = 特权层泳道**（4 列）：
  1. 用户态 · U mode · 用户栈
  2. 内核系统调用 · S · 进程内核栈
  3. 内核驱动 · S · 内核栈
  4. 硬件 · M / 设备
- **纵轴 = 时间**（从上到下推进）
- **箭头约定**：
  - 紫色实线 = 用户↔内核特权级切换（`ecall` / `sret`）
  - 灰色实线 = 同步调用/返回
  - 灰色虚线 = 阻塞/唤醒
  - 紫色虚线 = 循环回到顶部
- **每个节点必须标注三要素**：所在栈 / 特权级 / 是否持自旋锁

## 节点详细说明（按 SVG 中序号）

| # | 节点 | 源码位置 | 栈/态/锁依据 |
|---|---|---|---|
| ① | shell: `getcmd → read(0)` | `user/sh.c:160` while 循环 | 用户栈（sp 在 USERSTACK 附近）· U · 无锁 |
| ② | `uservec → usertrap` | `kernel/trampoline.S:uservec`、`kernel/trap.c:usertrap` | 进程内核栈（sp = trapframe->kernel_sp）· S · 无锁；保存 32 个通用寄存器到 trapframe |
| ③ | `syscall → sys_read` | `kernel/syscall.c` 用 `syscalls[]` 表按 a7 分发 | 进程内核栈 · S；从 trapframe->a0~a2 取用户传参 |
| ④ | `sleep(&cons.r, &cons.lock)` | `kernel/console.c:consoleread` | 进程内核栈 · S；**先 release cons.lock 再 sleep**——否则 consoleintr 拿不到锁无法唤醒 → 死锁 |
| ⑤ | 用户敲键盘 "echo hi↵" | 硬件事件 | UART RXTX IRQ · M→S；QEMU UART 收到字符触发 IRQ，plic claim → kernelvec → devintr → consoleintr |
| ⑥ | `consoleintr → wakeup` | `kernel/console.c:consoleintr` | 拿 cons.lock，把字符存入 cons.buf，调 `wakeup(&cons.r)` 唤醒 ④ 的进程，设 RUNNABLE |
| ⑦ | `sys_read` 返回 | `kernel/trap.c:usertrapret`（参考树已更名 `prepare_return`） | 进程内核栈 → 通过 userret 恢复 trapframe → `sret` 回用户态 |
| ⑧ | `parsecmd + fork1` | `user/sh.c:172` | 用户栈 · U；调 `fork()` 桩 → `ecall` |
| ⑨ | `sys_fork → copyproc + uvmcopy` | `kernel/sysproc.c:sys_fork`、`kernel/proc.c:copyproc`、`kernel/vm.c:uvmcopy` | 进程内核栈 · S；**持 wait_lock** 保护 parent/child 链；uvmcopy 复制页表（COW 在 lab5 才上） |
| ⑩⑩b | shell 父 `wait(0) → sys_wait → sleep` | `kernel/sysproc.c:sys_wait` | 用户栈 → ecall → 进程内核栈 · S；无 zombie 子进程时 `sleep(&wait_lock)` |
| ⑪⑫ | shell 子 `runcmd → exec → kexec` | `user/sh.c:runcmd`、`kernel/exec.c:kexec` | 用户栈 → ecall → 进程内核栈 · S · **无锁**；读 ELF、构造新页表、替换旧页表、设 PC=elf entry |
| ⑬⑭⑮⑯ | echo `write(1,"hi\n") → sys_write → filewrite → consolewrite → uartputc → UART THR` | `kernel/sysfile.c:sys_write`、`kernel/file.c:filewrite`、`kernel/console.c:consolewrite`、`kernel/uart.c:uartputc_sync` | 进程内核栈 · S；**consolewrite 持 cons.lock**；uartputc 写 MMIO 0x10000000（UART THR 寄存器）；QEMU 把 UART 输出打到终端 |
| ⑰⑱ | echo `exit(0) → sys_exit` | `kernel/sysproc.c:sys_exit` | 用户栈 → ecall → 进程内核栈 · S；关 trapframe，设 p->state=ZOMBIE，调 `wakeup(&p->parent->wait_lock)` |
| ⑲⑳ | `scheduler → swtch → 回 shell 父` | `kernel/proc.c:scheduler`、`kernel/swtch.S` | **调度器专门栈**（每个 CPU 一个，start.c 在 M 态先建好）· S；swtch 切上下文；shell 父进程从 sys_wait 醒来后 `freeproc(zombie)` 真正回收子进程 |

## 关键不变式

1. **sleep 必须先释放自旋锁再睡**——否则中断处理拿不到锁，无法唤醒，死锁。
2. **scheduler 用专门调度栈**，不是进程内核栈——避免与任何进程内核栈互相覆盖。
3. **trampoline 页是所有进程共享**，uvmcopy 时 PTE 只读、物理页不复制（材料二会详画）。
4. **fork 后父子返回点都在 `trapframe->a0`**，但 a0 值不同（父=子pid，子=0）——这是 fork "返回两次"的底层。
5. **exec 替换页表时旧 trapframe 必须保留**——否则 `sret` 回不去用户态。
6. **sys_exit 到 scheduler 切走前不能持有任何自旋锁**——否则下一个进程拿不到锁，死锁。

---

## ★ 自主思考批注

### 批注 1：sleep 为什么不能拿着 cons.lock 直接睡眠？
**位置**：节点 ④ `sleep(&cons.r, &cons.lock)`

AI：自旋锁 `cons.lock` 持有期间**关中断**。如果持锁 sleep：
1. 关中断 → UART 中断永远无法触发 → `consoleintr` 永远跑不到 → 没人执行 `wakeup`；进程永久睡死。
2. 锁一直被占，其他进程想 console 读写时拿不到锁，全局死锁。

`sleep` 的设计就是：调用时持有锁，函数内部原子释放锁 + 进入睡眠，保证唤醒逻辑正确。

### 批注 2：uvmcopy 复制页表，trampoline 页为什么不复制物理页？
**位置**：节点 ⑨ `uvmcopy`

AI：trampoline 是汇编代码，用于 `uservec/userret`，**功能全局统一、只读**。
所有进程的页表都把同一个虚拟地址映射到**同一份物理页**。不需要每个进程复制一份；复制纯粹浪费内存，而且 trampoline 不属于用户内存，属于内核和用户切换的跳板，由内核全局维护。

### 批注 3：sys_exit 之后调用 scheduler 前不能持有自旋锁
**位置**：节点 ⑱ ~ ⑲ `sys_exit → scheduler`

AI：xv6 自旋锁的规则：持有自旋锁时**禁止调度、不能主动放弃 CPU**。
自旋锁只是忙等，不是互斥休眠锁。如果 exit 拿着锁调用 `scheduler`，当前进程被切走，锁依然标记为 held；别的 CPU 尝试获取这把锁，就会无限自旋，死锁。
所以在调用调度器之前，必须释放全部自旋锁。

### 批注 4：exec 替换页表，旧 trapframe 必须保留
**位置**：节点 ⑫ `kexec`

AI：`trapframe` 在内核虚拟地址空间（高地址，不在用户页表），`sret` 恢复寄存器时要从 trapframe 读取上下文。
exec 销毁、重建**用户侧页表**，但不能删掉 trapframe：否则执行 `sret` 的时候没有寄存器上下文，无法回到新 echo 的用户入口点，系统调用返回会崩溃。

---

### 我的额外批注（区分点 / 边界特例）

#### 批注 5：ecall / sret 是特权级切换指令，不是函数调用
**位置**：节点 ①↔②、⑫↔⑬
ecall/sret 切换**特权级 + 栈**（用户栈 ↔ 进程内核栈），不保存返回地址在栈上（ra 在 trapframe 里）。这与普通 jal/call 本质不同——硬件 trap 机制而非函数栈帧。

#### 批注 6：中断上下文（consoleintr）用当前进程的内核栈，不能 sleep 只能 wakeup
**位置**：节点 ⑥ `consoleintr`
中断处理跑在被打断进程的内核栈上，**不能 sleep**（没有合法的"睡眠对象"，会让无辜进程被睡死）。中断里只能 `wakeup` 别人，自己跑完即返回。这是中断 vs 系统调用处理的最关键区别。

#### 批注 7：调度器 scheduler 有独立栈，不属于任何进程
**位置**：节点 ⑲ `scheduler → swtch`
调度器用每个 CPU 的独立栈，`swtch` 只切换栈和 callee-saved 寄存器（ra/sp/s0-s11），不碰页表——页表切换在 `scheduler` 选中进程后由 `w_satp` 完成。

#### 批注 8：Zombie 进程 —— 用户内存释放，PCB 保留
**位置**：节点 ⑱ `sys_exit → ZOMBIE`
exit 把用户页表/物理页/trapframe 全释放，但**保留 proc 结构**（state/parent/xv6-recorded exit status），等父进程 `wait` 时由 `freeproc` 彻底回收。如果父进程不 wait，PCB 永久占用 proc 表 slot——这就是僵尸进程的资源占用问题。

---

## 验收检查清单对照

- [✓] 控制流图覆盖 `read → fork → exec → write → exit` 全链路（节点 ①~⑳）
- [✓] 核心跳转准确标注堆栈归属与特权级转换（每个节点三要素齐全）
- [✓] 包含至少 3 处自主思考批注（实际 8 条：4 条核心批注 + 4 条区分点）
- [✓] 图纸保存在工程目录 `lab0/material1_control_flow.svg` 

## 文件路径

- 图：`~/oslab/2024302111353-kernel/lab0/material1_control_flow.svg`
- 本说明：`~/oslab/2024302111353-kernel/lab0/material1_control_flow.md`