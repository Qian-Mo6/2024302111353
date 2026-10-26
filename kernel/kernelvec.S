        # ============ lab3 阶段三 · 内核态陷入向量 ============
        # kernelvec:内核在 S 态执行期间,stvec 指向这里。
        #
        # 与 trampoline.S 的 uservec 分工(stvec 切换协议,规范 §2.2):
        #   - 用户态陷入:uservec 处理(切 satp 到内核页表后调 usertrap);
        #   - 内核态陷入:kernelvec 处理——页表已是内核页表、栈已是内核栈,
        #     故不切 satp、不换栈,直接在当前栈保存现场,调 kerneltrap()。
        #
        # 若缺少本向量:内核执行期(SIE=1 窗口)到达的中断会按 stvec
        # 再进 uservec,在内核页表下按用户语义向 TRAPFRAME 保存现场,
        # 覆盖已保存的用户现场并递归陷入,引发无法恢复的异常风暴。
        #
        # 只保存调用者保存寄存器(caller-saved);callee-saved 由
        # kerneltrap 的 C 代码按 ABI 自行保护。
        .globl kernelvec
        .align 4
kernelvec:
        addi sp, sp, -256

        sd ra, 0(sp)
        sd gp, 16(sp)
        sd tp, 24(sp)
        sd t0, 32(sp)
        sd t1, 40(sp)
        sd t2, 48(sp)
        sd a0, 56(sp)
        sd a1, 64(sp)
        sd a2, 72(sp)
        sd a3, 80(sp)
        sd a4, 88(sp)
        sd a5, 96(sp)
        sd a6, 104(sp)
        sd a7, 112(sp)
        sd t3, 120(sp)
        sd t4, 128(sp)
        sd t5, 136(sp)
        sd t6, 144(sp)

        call kerneltrap

        ld ra, 0(sp)
        ld gp, 16(sp)
        ld tp, 24(sp)
        ld t0, 32(sp)
        ld t1, 40(sp)
        ld t2, 48(sp)
        ld a0, 56(sp)
        ld a1, 64(sp)
        ld a2, 72(sp)
        ld a3, 80(sp)
        ld a4, 88(sp)
        ld a5, 96(sp)
        ld a6, 104(sp)
        ld a7, 112(sp)
        ld t3, 120(sp)
        ld t4, 128(sp)
        ld t5, 136(sp)
        ld t6, 144(sp)

        addi sp, sp, 256
        sret

        # ============ lab4 stage34 · M-mode 时钟中断向量 ============
        # MTIP 不委托(mideleg.MTIP=0),M-mode trap 从 mtvec=mtrapvec 进入。
        #   1. mtvec 低 2 位是模式位(00=direct):C 函数可能 2 字节对齐(RVC),
        #      直接取函数地址会把模式位污染成 10(保留),QEMU 按向量模式
        #      解析后跳到垃圾地址(PC=0 无限取指错) → .align 强制对齐;
        #   2. 被中断代码可能正处于 trampoline(用户页表 + 用户 sp):
        #      M-mode 访存不经 satp 翻译,沿用用户 sp 会把现场写到随机
        #      物理地址 → 先用 mscratch 暂存 sp,切到专用 M-mode 栈;
        #   3. 中断随时打断任意代码,必须保存全部调用者保存寄存器。
        .globl mtrapvec
        .align 4
mtrapvec:
        csrw mscratch, sp          # 暂存被打断代码的 sp
        la   sp, mstack_top        # 切到 M-mode 专用栈(链接地址即物理地址)
        addi sp, sp, -256
        sd   ra, 0(sp)
        sd   t0, 8(sp)
        sd   t1, 16(sp)
        sd   t2, 24(sp)
        sd   a0, 32(sp)
        sd   a1, 40(sp)
        sd   a2, 48(sp)
        sd   a3, 56(sp)
        sd   a4, 64(sp)
        sd   a5, 72(sp)
        sd   a6, 80(sp)
        sd   a7, 88(sp)
        sd   t3, 96(sp)
        sd   t4, 104(sp)
        sd   t5, 112(sp)
        sd   t6, 120(sp)
        call mtrap_c
        ld   ra, 0(sp)
        ld   t0, 8(sp)
        ld   t1, 16(sp)
        ld   t2, 24(sp)
        ld   a0, 32(sp)
        ld   a1, 40(sp)
        ld   a2, 48(sp)
        ld   a3, 56(sp)
        ld   a4, 64(sp)
        ld   a5, 72(sp)
        ld   a6, 80(sp)
        ld   a7, 88(sp)
        ld   t3, 96(sp)
        ld   t4, 104(sp)
        ld   t5, 112(sp)
        ld   t6, 120(sp)
        addi sp, sp, 256
        csrr sp, mscratch          # 恢复被打断代码的 sp
        mret

        # M-mode 专用中断栈(数据段,可写;链接地址 = 物理地址)
        .section .data
        .align 4
mstack:
        .zero 256
mstack_top:
        .section .text
