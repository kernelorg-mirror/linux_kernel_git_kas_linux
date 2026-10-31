/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Level-generic page table API: the types alone, for headers that embed or
 * pass entry pointers and values without operating on them. The operations
 * are in <linux/pgt.h>.
 */
#ifndef _LINUX_PGT_TYPES_H
#define _LINUX_PGT_TYPES_H

#include <linux/types.h>

#define PGT_MAX_LEVEL		(CONFIG_PGTABLE_LEVELS - 1)

/* Wide enough for an entry at any level: PAE and LPAE entries are 64-bit */
#if defined(CONFIG_PGT_128BIT)
typedef u128 pgtval_t;
#elif defined(CONFIG_64BIT)
typedef unsigned long pgtval_t;
#else
typedef u64 pgtval_t;
#endif

/**
 * typedef pgtp_t - Pointer to a page table entry, with the level of its table.
 *
 * Generic code uses the pgtp_*() helpers and does not read the fields.
 */
typedef struct pgtp {
	void *ptr;
	unsigned int lvl;
} pgtp_t;

/**
 * typedef pgtv_t - Snapshot of a page table entry of any kind.
 *
 * @val is the raw entry and is opaque to generic code. @lvl is the level
 * the value belongs to; it travels with the value because the meaning of
 * the bits depends on it.
 */
typedef struct pgtv {
	pgtval_t val;
	unsigned int lvl;
} pgtv_t;

/**
 * typedef pgt_leaf_t - Snapshot of a LEAF entry.
 *
 * Only the pgt_leaf_*() functions take it, so a leaf accessor cannot be
 * applied to another kind. Made by pgtv_to_leaf() once pgtv_kind() said
 * LEAF, or by pgt_leaf_mk().
 */
typedef struct pgt_leaf {
	pgtv_t v;
} pgt_leaf_t;

/**
 * typedef pgt_softleaf_t - Snapshot of a SOFTLEAF entry.
 *
 * The encoded entry: besides the softleaf_t it holds the soft-dirty,
 * uffd-wp and exclusive bits. Made by pgtv_to_softleaf() or
 * pgt_softleaf_mk().
 */
typedef struct pgt_softleaf {
	pgtv_t v;
} pgt_softleaf_t;

/**
 * typedef pgt_table_t - Snapshot of a TABLE entry.
 *
 * Made by pgtv_to_table() or pgt_table_mk().
 */
typedef struct pgt_table {
	pgtv_t v;
} pgt_table_t;

/**
 * enum pgt_kind - What a page table entry is.
 * @PGT_NONE: empty.
 * @PGT_TABLE: links the table one level down.
 * @PGT_LEAF: maps memory of the entry's size; Linux-present, which
 *            includes PROT_NONE and invalidated entries.
 * @PGT_SOFTLEAF: software entry (swap, migration, device-private, marker).
 * @PGT_BAD: none of the above.
 */
enum pgt_kind {
	PGT_NONE,
	PGT_TABLE,
	PGT_LEAF,
	PGT_SOFTLEAF,
	PGT_BAD,
};

#define PGTP_NULL		((pgtp_t){ 0 })

#endif /* _LINUX_PGT_TYPES_H */
