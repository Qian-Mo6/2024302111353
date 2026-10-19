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
