/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Level-generic page table walkers.
 *
 * PGT_DEFINE_LEVEL_WALKER() turns one function body that handles any level
 * into one function per level, so that each instance is compiled with a
 * constant level. struct pgt_iter is the flattened form, for a consumer
 * that wants to own the loop. Both are conveniences over the primitives in
 * <linux/pgt.h>; see Documentation/mm/pgt.rst.
 */
#ifndef _LINUX_PGT_WALK_H
#define _LINUX_PGT_WALK_H

#include <linux/pgt.h>

#define __PGT_WALKER_LVL(k)	((k) > PGT_MAX_LEVEL ? PGT_MAX_LEVEL : (k))

#define __PGT_WALKER_INSTANCE(name, body, ctx_t, k)			\
static unsigned long __maybe_unused					\
name##_l##k(ctx_t ctx, pgtp_t p, pgtp_t pp, unsigned long addr,		\
	    unsigned long end)						\
{									\
	pgtp_assume_level(p, __PGT_WALKER_LVL(k));			\
	if (!(k))							\
		pgtp_assume_level(pp, 1);				\
	return body(ctx, p, pp, addr, end, __PGT_WALKER_LVL(k));	\
}

/**
 * PGT_DEFINE_LEVEL_WALKER() - Instantiate a walker body for every level.
 * @name: name of the dispatcher; instances are @name_l0 to @name_l4.
 * @body: the body, an __always_inline function
 *        ``unsigned long body(ctx_t ctx, pgtp_t p, pgtp_t pp,
 *        unsigned long addr, unsigned long end, const unsigned int lvl)``.
 * @ctx_t: type of the walker context.
 *
 * Declares @body, emits one instance per level and an __always_inline
 * dispatcher ``@name(ctx, p, pp, addr, end, lvl)`` that calls the instance
 * for @lvl; with a constant @lvl the call is direct. The dispatcher never
 * names an instance above PGT_MAX_LEVEL, so those generate no code even
 * when @lvl is not known at build time. Each instance passes its level to
 * pgtp_assume_level(), so that the operations in @body compile for that
 * level alone; the level-0 instance states the level of @pp as well. The
 * macro may precede the definition of @body, so that the body descends by
 * calling @name with ``lvl - 1``.
 *
 * The body handles the entries of one table from @addr to @end, @p being
 * the entry for @addr and @pp the entry the table was reached through,
 * PGTP_NULL only at the root. It returns the address it reached: @end when
 * done, anything else when it stopped early. The caller of the root call
 * decides whether to restart from that address or to give up.
 */
#define PGT_DEFINE_LEVEL_WALKER(name, body, ctx_t)			\
static __always_inline unsigned long					\
body(ctx_t ctx, pgtp_t p, pgtp_t pp, unsigned long addr,		\
		unsigned long end, const unsigned int lvl);		\
__PGT_WALKER_INSTANCE(name, body, ctx_t, 0)				\
__PGT_WALKER_INSTANCE(name, body, ctx_t, 1)				\
__PGT_WALKER_INSTANCE(name, body, ctx_t, 2)				\
__PGT_WALKER_INSTANCE(name, body, ctx_t, 3)				\
__PGT_WALKER_INSTANCE(name, body, ctx_t, 4)				\
static __always_inline unsigned long					\
name(ctx_t ctx, pgtp_t p, pgtp_t pp, unsigned long addr,		\
		unsigned long end, unsigned int lvl)			\
{									\
	if (!lvl)							\
		return name##_l0(ctx, p, pp, addr, end);		\
	if (lvl == 1 || PGT_MAX_LEVEL == 1)				\
		return name##_l1(ctx, p, pp, addr, end);		\
	if (lvl == 2 || PGT_MAX_LEVEL == 2)				\
		return name##_l2(ctx, p, pp, addr, end);		\
	if (lvl == 3 || PGT_MAX_LEVEL == 3)				\
		return name##_l3(ctx, p, pp, addr, end);		\
	return name##_l4(ctx, p, pp, addr, end);			\
}

/**
 * struct pgt_iter - State of a flattened walk.
 * @mm: the mm.
 * @addr: address of the current entry.
 * @end: end of the walk.
 * @lvl: level of the current entry.
 * @root: level of the root entry the walk started from.
 * @flags: PGT_ITER_* flags.
 * @path: entry pointers from the root down to @lvl.
 * @cur: snapshot of the current entry.
 * @ptl: level-0 table lock held with PGT_ITER_LOCKED, or NULL.
 */
struct pgt_iter {
	struct mm_struct *mm;
	unsigned long addr, end;
	unsigned int lvl, root, flags;
	pgtp_t path[PGT_MAX_LEVEL + 1];
	pgtv_t cur;
	spinlock_t *ptl;
};

/* Walk under RCU without page table locks */
#define PGT_ITER_RO		0x1
/* Hold the level-0 table lock while yielding its entries */
#define PGT_ITER_LOCKED		0x2

/**
 * PGT_ITER() - Declare and initialise an iterator.
 * @name: variable name.
 * @_mm: the mm.
 * @start: first address.
 * @_end: end of the walk.
 * @_flags: PGT_ITER_* flags.
 */
#define PGT_ITER(name, _mm, start, _end, _flags)			\
	struct pgt_iter name = {					\
		.mm = (_mm),						\
		.addr = (start),					\
		.end = (_end),						\
		.flags = (_flags),					\
	}

/**
 * pgt_iter_first() - Start a walk.
 * @it: the iterator.
 *
 * Return: the first non-TABLE entry at or after the start address.
 */
pgtv_t pgt_iter_first(struct pgt_iter *it);

/**
 * pgt_iter_next() - Move to the next entry.
 * @it: the iterator.
 *
 * Descends through TABLE entries; yields LEAF, SOFTLEAF, NONE and BAD
 * entries at the level where they are found. With PGT_ITER_LOCKED a lost
 * lock makes the iterator walk again from the current address.
 *
 * Return: the next non-TABLE entry.
 */
pgtv_t pgt_iter_next(struct pgt_iter *it);

/**
 * pgt_iter_done() - Check for the end of the walk.
 * @it: the iterator.
 *
 * Return: true once the walk has passed its end; locks are released then.
 */
bool pgt_iter_done(struct pgt_iter *it);

/**
 * pgt_iter_addr() - Address of the current entry.
 * @it: the iterator.
 *
 * Return: the address, clipped to the start of the walk.
 */
unsigned long pgt_iter_addr(struct pgt_iter *it);

/**
 * pgt_iter_size() - Span of the current entry.
 * @it: the iterator.
 *
 * Return: bytes from pgt_iter_addr() to the end of the entry, clipped to
 * the end of the walk. For a NONE entry this is the size of the hole.
 */
unsigned long pgt_iter_size(struct pgt_iter *it);

/**
 * pgt_iter_ptp() - Pointer to the current entry.
 * @it: the iterator.
 *
 * Return: the entry pointer.
 */
pgtp_t pgt_iter_ptp(struct pgt_iter *it);

#endif /* _LINUX_PGT_WALK_H */
