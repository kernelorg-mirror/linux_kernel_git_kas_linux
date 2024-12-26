/* SPDX-License-Identifier: GPL-2.0 */
/*
 * These are in machine order; things rely on that.
 */
GEN(eax, 0)
GEN(ecx, 1)
GEN(edx, 2)
GEN(ebx, 3)
GEN(esp, 4)
GEN(ebp, 5)
GEN(esi, 6)
GEN(edi, 7)
#ifdef CONFIG_64BIT
GEN(r8d,  8)
GEN(r9d,  9)
GEN(r10d, 10)
GEN(r11d, 11)
GEN(r12d, 12)
GEN(r13d, 13)
GEN(r14d, 14)
GEN(r15d, 15)
#ifdef CONFIG_X86_KERNEL_APX
GEN(r16d, 16)
GEN(r17d, 17)
GEN(r18d, 18)
GEN(r19d, 19)
GEN(r20d, 20)
GEN(r21d, 21)
GEN(r22d, 22)
GEN(r23d, 23)
GEN(r24d, 24)
GEN(r25d, 25)
GEN(r26d, 26)
GEN(r27d, 27)
GEN(r28d, 28)
GEN(r29d, 29)
GEN(r30d, 30)
GEN(r31d, 31)
#endif
#endif
