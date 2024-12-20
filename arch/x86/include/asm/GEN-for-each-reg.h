/* SPDX-License-Identifier: GPL-2.0 */
/*
 * These are in machine order; things rely on that.
 */
#ifdef CONFIG_64BIT
GEN(rax, 0)
GEN(rcx, 1)
GEN(rdx, 2)
GEN(rbx, 3)
GEN(rsp, 4)
GEN(rbp, 5)
GEN(rsi, 6)
GEN(rdi, 7)
GEN(r8,  8)
GEN(r9,  9)
GEN(r10, 10)
GEN(r11, 11)
GEN(r12, 12)
GEN(r13, 13)
GEN(r14, 14)
GEN(r15, 15)
#ifdef CONFIG_X86_KERNEL_APX
GEN(r16, 16)
GEN(r17, 17)
GEN(r18, 18)
GEN(r19, 19)
GEN(r20, 20)
GEN(r21, 21)
GEN(r22, 22)
GEN(r23, 23)
GEN(r24, 24)
GEN(r25, 25)
GEN(r26, 26)
GEN(r27, 27)
GEN(r28, 28)
GEN(r29, 29)
GEN(r30, 30)
GEN(r31, 31)
#endif
#else
GEN(eax, 0)
GEN(ecx, 1)
GEN(edx, 2)
GEN(ebx, 3)
GEN(esp, 4)
GEN(ebp, 5)
GEN(esi, 6)
GEN(edi, 7)
#endif
