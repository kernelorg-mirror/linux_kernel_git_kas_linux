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
