/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __VDSO_PAGE_H
#define __VDSO_PAGE_H

#include <uapi/linux/const.h>

/*
 * PTE_SHIFT determines the MMU page size.
 *
 * Note: This definition is required because PTE_SHIFT is used
 * in several places throughout the codebase.
 */
#define PTE_SHIFT      CONFIG_PTE_SHIFT

#define PTE_SIZE	(_AC(1,UL) << CONFIG_PTE_SHIFT)

#if !defined(CONFIG_64BIT)
/*
 * Applies only to 32-bit architectures.
 *
 * Subtle: (1 << CONFIG_PTE_SHIFT) is an int, not an unsigned long.
 * So if we assign PTE_MASK to a larger type it gets extended the
 * way we want (i.e. with 1s in the high bits) while masking a
 * 64-bit value such as phys_addr_t.
 */
#define PTE_MASK	(~((1 << CONFIG_PTE_SHIFT) - 1))
#else
#define PTE_MASK	(~(PTE_SIZE - 1))
#endif

/* PG_SHIFT determines the size of order-0 buddy allocation */
#define PG_SHIFT	CONFIG_PG_SHIFT

#define PG_SIZE		(_AC(1,UL) << CONFIG_PG_SHIFT)

#if !defined(CONFIG_64BIT)
/* See comment for PTE_MASK */
#define PG_MASK		(~((1 << CONFIG_PG_SHIFT) - 1))
#else
#define PG_MASK		(~(PG_SIZE - 1))
#endif

/*
 * PAGE_SHIFT/SIZE/MASK have double meaning. They can be used for both
 * MMU page size and for order-0 buddy allocations.
 *
 * Define them as aliases of PG_* either when the two sizes match, or
 * when CONFIG_PAGE_DEFS_RELAXED is set so unaudited code keeps building
 * with PG_SIZE != PTE_SIZE.
 *
 * To be removed once transition is complete.
 */
#if PTE_SHIFT == PG_SHIFT || defined(CONFIG_PAGE_DEFS_RELAXED)
#define PAGE_SHIFT	PG_SHIFT
#define PAGE_SIZE	PG_SIZE
#define PAGE_MASK	PG_MASK
#endif

#endif	/* __VDSO_PAGE_H */
