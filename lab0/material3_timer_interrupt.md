# 材料三 · 一次时钟中断的微观旅程

> lab0 实验交付物。追踪从用户态跑 echo 时被时钟中断打断、到 yield + swtch 切换到调度器、再被切回继续执行的全过程。
> 配套图：`material3_timer_interrupt.svg`（同目录，纵向时序图 + 寄存器现场侧边）。

## 截面时刻定义

- **起点**：echo 进程正在用户态跑某条指令（PC 在代码段内）
- **触发**：CLINT 时钟达到 mtimecmp，硬件产生 S-mode timer interrupt
- **关键 scause**：`0x8000000000000005`
  - bit 63 = 1 → 表示这是中断（interrupt）而非异常（exception）
  - 低 4 位 = 5 → S-mode timer interrupt（supervisor timer）
- **终点**：echo 通过 sret 回到用户态，PC = trapframe.epc（原被打断的指令地址）

## 10 阶段时序对照表

| # | 阶段 | 所在栈/态 | 源码位置 | 关键操作 |
|---|---|---|---|---|
| ① | echo main 执行中 | 用户栈 / U | user/echo.c | PC 在代码段某条指令；CPU 跑 a0~a7/ra/sp |
| ② | 时钟硬件触发 IRQ | M 模式硬件 | CLINT | `mtime > mtimecmp` → supervisor timer IRQ |
| ③ | uservec | trampoline 页 / S | kernel/trampoline.S | csrrw 切 sp 到 trapframe；存 32 regs；sp = kernel_sp；jr usertrap |
| ④ | usertrap | 进程内核栈 / S | kernel/trap.c:38 | 校验 SPP=0；w_stvec(kernelvec)；记 epc；调 devintr |
| ⑤ | devintr → clockintr | 进程内核栈 / S | kernel/trap.c:188 | scause==0x...05 → clockintr()；设下次 mtimecmp；return 2 |
| ⑥ | yield → sched | 进程内核栈 / S | kernel/proc.c:501, 480 | acquire p->lock；p->state=RUNNABLE；swtch 保存 callee-saved |
| ⑦ | scheduler | 调度器专门栈 / S | kernel/proc.c:429 | 找下一个 RUNNABLE；w_satp 切页表；p->state=RUNNING；swtch 切上下文 |
| ⑧ | sched 返回 → yield 返回 | 进程内核栈 / S | kernel/proc.c:495 | swtch ret 回到 sched 调用者 yield；release p->lock |
| ⑨ | prepare_return | 进程内核栈 / S | kernel/trap.c:101 | 关中断；w_stvec(uservec)；填 trapframe.kernel_trap；jr userret |
| ⑩ | 回到 echo 用户态 | 用户栈 / U | kernel/trampoline.S | userret 恢复 32 regs；sret → U mode；PC = trapframe.epc |

## 关键寄存器现场表

### 中断发生时刻（阶段 ② → ③）

| CSR | 值 | 含义 |
|---|---|---|
| sstatus.SPP | 0 | 表示中断来自 U 模式（用户态被打断） |
| sstatus.SIE | 0 | 进入 trap 时硬件自动清零（关 S 中断） |
| stvec | uservec 地址 | 用户态中断向量，指向 TRAMPOLINE |
| sscratch | &p->trapframe | uservec 用它找到 trapframe 物理页 |
| sepc | 被打断指令的 PC | uservec 不直接读，usertrap 里 `r_sepc()` 存入 trapframe |
| scause | 0x8000000000000005 | S-mode timer interrupt 标识 |

### uservec 内部关键操作（阶段 ③）

```asm
uservec:
    csrrw  sp, sscratch, sp    # sp 原是用户栈，现为 trapframe
    sd     ra, 40(a0)          # 存 32 个通用寄存器到 trapframe
    ...
    csrr   t0, sscratch        # t0 = trapframe 地址
    ld     sp, 8(t0)           # sp = trapframe->kernel_sp（切到内核栈）
    ld     t0, 0(t0)           # t0 = trapframe->kernel_trap = usertrap
    jr     t0                  # 跳到 usertrap
```

### swtch 保存的寄存器（阶段 ⑥ → ⑦）

| 寄存器 | 是否保存 | 原因 |
|---|---|---|
| ra | ✓ | 调用者返回地址 |
| sp | ✓ | 栈指针 |
| s0~s11（12 个） | ✓ | callee-saved，调用者依赖 |
| a0~a7（8 个） | ✗ | caller-saved，调用者负责 |
| t0~t6（7 个） | ✗ | caller-saved，调用者负责 |
| gp / tp | ✗ | 全局指针 / CPU 指针 |

共 14 个 × 8 字节 = 112 字节。

## 调用栈对照（阶段 ⑥）

```
usertrap()                              <- 跑在进程内核栈
  └─ which_dev = devintr() == 2
  └─ yield()                            <- trap.c:86
       └─ acquire(&p->lock)
       └─ p->state = RUNNABLE
       └─ sched()                       <- proc.c:480
            └─ (sanity check: locks/state/intr)
            └─ swtch(&p->context,        <- swtch.S
                     &mycpu()->context)  ★ 切到调度器栈
```

切到调度器栈后，CPU 跑到 scheduler() 的某条指令（swtch 的 ret 之后），从那里继续：

```
scheduler()                             <- 跑在调度器专门栈
  └─ for(;;) {
       找 RUNNABLE 进程 p
       acquire(&p->lock)
       p->state = RUNNING
       w_satp(p->pagetable)             <- 切地址空间
       swtch(&c->context,               <- proc.c:453
              &p->context)              ★ 切回进程内核栈
       release(&p->lock)
     }
```

切回进程内核栈后，CPU 跑到 sched() 内 swtch 调用之后那条指令（即 `swtch` 的 ret），从那里继续：

```
sched() return                          <- proc.c:495 之后
yield()                                 <- proc.c:507
  └─ release(&p->lock)
usertrap()                               <- 回到 trap.c:88
  └─ prepare_return()
       └─ ...
       └─ jr userret (trampoline.S)
            └─ sret                     ★ 回到用户态
```

## 关键不变式（验收可能问到的）

1. **swtch 只切栈和 callee-saved**：不碰页表（页表切换在 scheduler 选中进程后由 `w_satp` 完成）、不碰 trapframe、不保存 caller-saved。
2. **scheduler 用专门调度栈**：不是任何进程的内核栈。如果 scheduler 跑在进程内核栈上，下一个被调度切走的进程会覆盖这个栈，调度器就崩了。
3. **持有 p->lock 才能 swtch**：yield 持锁 → sched → swtch → 切走；scheduler swtch 切回后 release。锁保护 state 字段不被并发改。
4. **中断返回时 stvec 要切回 uservec**：usertrap 一开始就把 stvec 改成 kernelvec，因为如果内核态又被中断（嵌套），不能用 uservec 处理。prepare_return 时再切回 uservec，准备 sret 回用户态。
5. **中断触发时 sstatus.SIE 自动清零**：硬件 trap 时关 S 中断，usertrap 跑完才在 prepare_return 里通过 sstatus 恢复。这避免了中断处理过程中被打断。
6. **scause 高位 = 1 表示中断**：bit 63 = interrupt (vs exception)，低 4 位标识具体类型（5 = S-timer / 9 = S-external / 8 = U-software / ...）。

---

## ★ 自主思考批注

### 批注 1：usertrap 一开始就 w_stvec(kernelvec) 是为什么？
**位置**：阶段 ④ usertrap

AI：用户态陷阱进来时 stvec = uservec，uservec 的逻辑依赖"当前是用户栈、需要借助 sscratch 交换栈"；一旦已经切换到内核栈，后续若在内核执行过程中再次触发中断，必须走 kernelvec。kernelvec 直接使用当前内核栈，不会执行 `csrrw sp, sscratch, sp`，避免覆盖 trapframe。所以 usertrap 第一时间更新 stvec，区分【用户态陷阱入口】和【内核态陷阱入口】两条不同汇编路径，防止嵌套中断时栈被破坏。

### 批注 2：scheduler 不持有任何进程的 p->lock，为什么 yield 必须持锁？
**位置**：阶段 ⑥ yield

AI：`p->lock` 用来保护进程状态 `p->state`，防止多核并发读写产生竞争。yield 修改 `p->state = RUNNABLE`、随后进入 swtch，锁保证状态更新对其他 CPU 调度器原子可见；同时 xv6 的不变量：**从进程内核线程切走时，必须持有该进程的 p->lock**。scheduler 是调度线程，不属于任何用户进程，所以它不长期持有进程锁，仅在选中目标进程、准备 `swtch` 切入前获取锁。

### 批注 3：为什么 swtch 不保存所有寄存器？
**位置**：阶段 ⑥ swtch

AI：遵循 RISC-V ABI。`swtch` 是被 yield/scheduler 调用的普通函数，只需要保存 **callee-saved 寄存器（ra, sp, s0~s11，共 14 个）**；caller-saved（a/t 组）由调用方负责，调用方不依赖它们在函数返回后保持原值，无需 swtch 保存。`swtch` 保存的是**内核上下文（context）**，不是用户寄存器。

### 批注 4：sret 之后 CPU 真的能精确地继续原指令吗？
**位置**：阶段 ⑩ → ①：sret 回用户态

AI：可以精确恢复。硬件收到中断时，在**当前指令提交之前**把该指令地址写入 sepc；usertrap 将 sepc 存入 trapframe->epc。返回路径 userret 将 epc 写回 sepc，sret 跳转至 sepc，同时从 trapframe 恢复全部 32 个用户寄存器。用户进程看不到中断、调度的发生，就好像连续执行，实现时间片抢占的透明性。

---

### 我的额外批注（对比/分类总结）

#### 批注 5：`struct trapframe` VS `struct context`
**位置**：全图

- **trapframe**：保存**用户态寄存器**（全部 32 个通用寄存器 + epc 等），用户态发生 trap 时填充，userret 恢复。属于**进程用户上下文**，每个进程 1 份。
- **context**：仅保存**内核 callee-saved 寄存器（14 个）**，在 `swtch` 中保存/恢复；记录进程在内核栈上被切走时的执行点。属于**进程内核线程上下文**。

总结：trapframe 存 U 态现场；context 存 S 态内核线程现场。

#### 批注 6：`uservec` VS `kernelvec`

- **uservec**：**用户态陷阱入口**。执行 `csrrw sp, sscratch, sp` 交换栈，切换到内核栈，保存全部用户寄存器；stvec 在**即将返回用户态前**设置为 uservec。
- **kernelvec**：**内核态陷阱入口**。已经在内核栈，不交换栈，只保存内核临时寄存器；进入 usertrap 后 stvec 设置为 kernelvec。

总结：uservec = U→S 跳板，带栈交换；kernelvec = S 内部陷阱处理，无栈交换。

#### 批注 7：yield 线程 VS scheduler 线程

- **yield**：属于**当前用户进程的内核线程**，运行在该进程的内核栈；负责把进程标记 RUNNABLE，调用 swtch 交出 CPU；持有 p->lock。
- **scheduler**：独立调度线程，运行在**每个 CPU 专属调度栈**，不属于任何用户进程；循环遍历 proc 数组，寻找 RUNNABLE 进程；不长期持有进程锁。

总结：yield 是进程主动放弃 CPU；scheduler 是全局选下一个进程。

#### 批注 8：硬件 trap（U→S） VS swtch 软件上下文切换

- **硬件 trap（时钟中断）**：CPU 硬件自动触发，修改 CSR（sepc/scause/sstatus），跳转到 stvec 指定地址；特权级切换 U→S。
- **swtch**：纯汇编软件函数，**不会改变 CPU 特权级（始终在内核 S 态）**，只切换内核线程的 context、切换 satp 页表，实现内核线程之间切换。

总结：trap = 硬件、U↔S、改特权；swtch = 软件、S 内部、不改特权。

---

## 验收检查清单对照

- [✓] 时序图覆盖：硬件中断 → uservec → usertrap → devintr → clockintr → yield → sched → swtch → scheduler → 切回 → prepare_return → userret → sret 全链路（10 阶段）
- [✓] 标注每阶段所在栈与特权级（用户栈/进程内核栈/调度器专门栈，U/S/M）
- [✓] 标注关键 CSR 现场变化（sstatus/stvec/sepc/scause/sscratch）
- [✓] 标注 swtch 保存的寄存器集合（ra/sp/s0~s11 共 14 个）
- [✓] 包含至少 3 处自主思考批注（实际 **8 条**：4 条核心批注 + 4 条对比/分类总结）
- [✓] 截面时刻定义清楚（scause=0x8000000000000005 = S-mode timer interrupt）
- [✓] 图纸已保存到 `~/oslab/2024302111353-kernel/lab0/material3_timer_interrupt.svg`

## 文件路径

- 图：`~/oslab/2024302111353-kernel/lab0/material3_timer_interrupt.svg`
- 本说明：`~/oslab/2024302111353-kernel/lab0/material3_timer_interrupt.md`、
