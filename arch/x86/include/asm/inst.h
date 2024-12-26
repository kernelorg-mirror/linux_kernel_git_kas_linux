/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Generate .byte code for some instructions not supported by old
 * binutils.
 */
#ifndef X86_ASM_INST_H
#define X86_ASM_INST_H

#ifdef __ASSEMBLY__

#define REG_NUM_INVALID		100

#define REG_TYPE_R32		0
#define REG_TYPE_R64		1
#define REG_TYPE_INVALID	100

	.macro R32_NUM opd r32
	\opd = REG_NUM_INVALID

#define GEN(reg, regno)		\
	.ifc \r32, ## reg	\
	\opd = regno		\
	.endif
#include <asm/GEN-for-each-dword-reg.h>
#undef GEN

	.endm

	.macro R64_NUM opd r64
	\opd = REG_NUM_INVALID

#define GEN(reg, regno)		\
	.ifc \r32, ## reg	\
	\opd = regno		\
	.endif
#include <asm/GEN-for-each-reg.h>
#undef GEN

	.endm

	.macro REG_TYPE type reg
	R32_NUM reg_type_r32 \reg
	R64_NUM reg_type_r64 \reg
	.if reg_type_r64 <> REG_NUM_INVALID
	\type = REG_TYPE_R64
	.elseif reg_type_r32 <> REG_NUM_INVALID
	\type = REG_TYPE_R32
	.else
	\type = REG_TYPE_INVALID
	.endif
	.endm

	.macro PFX_REX opd1 opd2 W=0
	.if ((\opd1 | \opd2) & 8) || \W
	.byte 0x40 | ((\opd1 & 8) >> 3) | ((\opd2 & 8) >> 1) | (\W << 3)
	.endif
	.endm

	.macro MODRM mod opd1 opd2
	.byte \mod | (\opd1 & 7) | ((\opd2 & 7) << 3)
	.endm
#endif

#endif
