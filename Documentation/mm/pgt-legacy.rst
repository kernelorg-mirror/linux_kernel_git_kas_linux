.. SPDX-License-Identifier: GPL-2.0

========================================
pgt and the per-level page table helpers
========================================

Core mm code walks and edits user page tables through five families of
helpers, one per level: ``pgd_*``, ``p4d_*``, ``pud_*``, ``pmd_*`` and
``pte_*``. Each family repeats the same operations for its own entry type,
and each walker repeats the same loop once per level. A change that should
apply at every level (a new accessor, a lock rule, a wider entry) has to be
made five times and usually is made at two or three.

:doc:`pgt` replaces the families, for the code that uses it, with one
entry pointer type and a few entry value types that carry the level as
data. This document is about the relation between the two: which helper
each pgt operation stands for, how the fallback builds pgt on top of the
helpers, where the two disagree, and in which order the helpers go away.


Mapping
=======

The pgt operation and the per-level helpers it stands for. Levels 0, 1 and
2 are the PTE, PMD and PUD levels.

===============================================  ==============================================
pgt                                              Per-level helpers
===============================================  ==============================================
**Types**
``pgtp_t``                                       ``pte_t *``, ``pmd_t *``, ``pud_t *``,
                                                 ``p4d_t *``, ``pgd_t *``
``pgtv_t``                                       ``pte_t``, ``pmd_t``, ``pud_t``, ``p4d_t``,
                                                 ``pgd_t``
``pgt_leaf_t``                                   a present ``pte_t``, a huge ``pmd_t`` or
                                                 ``pud_t``
``pgt_softleaf_t``                               a non-present, non-empty ``pte_t`` or
                                                 ``pmd_t``
``pgt_table_t``                                  a ``pmd_t``, ``pud_t``, ``p4d_t`` or ``pgd_t``
                                                 that links a table
**Walking**
``pgtp_root()``                                  ``pgd_offset()``, then ``p4d_offset()`` and
                                                 ``pud_offset()`` through folded levels
``pgt_table_child()``                            ``pte_offset_map()``, ``pmd_offset()``,
                                                 ``pud_offset()``, ``p4d_offset()``
``pgtp_next()``, ``pgtp_advance()``              pointer arithmetic on ``pte_t *`` and kin
``pgtp_addr_end()``                              ``pmd_addr_end()`` and kin
``pgtp_get()``                                   ``ptep_get()``, ``pmdp_get()``,
                                                 ``pudp_get()``, ``p4dp_get()``, ``pgdp_get()``
``pgtp_get_lockless()``                          ``ptep_get_lockless()``,
                                                 ``pmdp_get_lockless()``
``PGT_DEFINE_LEVEL_WALKER()``                    one function per level, such as
                                                 ``zap_pmd_range()`` and ``zap_pte_range()``
``struct pgt_iter``                              ``walk_page_range()`` and its callbacks
**Kinds**
``PGT_NONE``                                     ``pte_none()``, ``pmd_none()`` and kin
``PGT_LEAF``                                     ``pte_present()``, ``pmd_leaf()``,
                                                 ``pud_leaf()``
``PGT_SOFTLEAF``                                 not ``pte_present()`` and not ``pte_none()``;
                                                 a non-present huge ``pmd_t``
``PGT_BAD``                                      ``pmd_bad()`` and kin
``pgtv_same()``                                  ``pte_same()``, ``pmd_same()``, ``pud_same()``
**Stores**
``pgtp_set_leaf()``                              ``set_pte_at()``, ``set_pmd_at()``,
                                                 ``set_pud_at()``
``pgtp_set_leaf_nr()``                           ``set_ptes()``
``pgtp_set_softleaf()``                          ``set_pte_at()``, ``set_pmd_at()``
``pgtp_clear()``                                 ``pte_clear()``, ``pmd_clear()`` and kin
``pgtp_xchg()``                                  ``ptep_get_and_clear()``,
                                                 ``pmdp_huge_get_and_clear()``,
                                                 ``pudp_huge_get_and_clear()``,
                                                 ``pmdp_establish()``
``pgtp_invalidate()``                            ``pmdp_invalidate()``, ``pudp_invalidate()``
``pgtp_get_and_clear_leaves()``                  ``get_and_clear_full_ptes()``,
                                                 ``pmdp_huge_get_and_clear_full()``,
                                                 ``pudp_huge_get_and_clear_full()``
``pgtp_clear_leaves()``                          ``clear_full_ptes()``, PMD and PUD forms
``pgtp_clear_softleaves()``                      ``pte_clear()``,
                                                 ``pmdp_huge_get_and_clear_full()``
``pgt_alloc_child()``                            ``__pte_alloc()``, ``__pmd_alloc()``,
                                                 ``__pud_alloc()``, ``__p4d_alloc()``
``pgtp_clear_bad()``                             ``pmd_clear_bad()`` and kin
**Leaves**
``pgt_leaf_mk()``                                ``pfn_pte()``; ``pfn_pmd()`` and ``pfn_pud()``
                                                 with ``pmd_mkhuge()`` and ``pud_mkhuge()``
``pgt_leaf_pfn()``                               ``pte_pfn()``, ``pmd_pfn()``, ``pud_pfn()``
``pgt_leaf_pgprot()``                            ``pte_pgprot()``, ``pmd_pgprot()``,
                                                 ``pud_pgprot()``
``pgt_leaf_modify()``                            ``pte_modify()``, ``pmd_modify()``,
                                                 ``pud_modify()``
``pgt_leaf_write()`` and its modifiers           ``pte_write()``, ``pte_mkwrite()``,
                                                 ``pte_mkwrite_novma()``, ``pte_wrprotect()``,
                                                 PMD and PUD forms
``pgt_leaf_dirty()`` and its modifiers           ``pte_dirty()``, ``pte_mkdirty()``,
                                                 ``pte_mkclean()``, PMD and PUD forms
``pgt_leaf_young()`` and its modifiers           ``pte_young()``, ``pte_mkyoung()``,
                                                 ``pte_mkold()``, PMD and PUD forms
``pgt_leaf_special()`` and its modifier          ``pte_special()``, ``pte_mkspecial()``, PMD
                                                 and PUD forms
``pgt_leaf_soft_dirty()`` and its modifiers      ``pte_soft_dirty()``, ``pte_mksoft_dirty()``,
                                                 ``pte_clear_soft_dirty()``, PMD forms
``pgt_leaf_uffd_wp()`` and its modifiers         ``pte_uffd()``, ``pte_mkuffd()``,
                                                 ``pte_clear_uffd()``, PMD forms
``pgt_leaf_exec()``                              no common helper
``pgt_leaf_user()``                              no common helper
``pgt_leaf_cont()``                              no common helper
``pgt_leaf_accessible()``                        ``pte_access_permitted()``,
                                                 ``pmd_access_permitted()``,
                                                 ``pud_access_permitted()``
``pgt_leaf_next()``                              ``pte_advance_pfn(pte, 1)``
``pgt_leaf_size()``                              ``PAGE_SIZE``, ``HPAGE_PMD_SIZE``,
                                                 ``HPAGE_PUD_SIZE``
``pgt_leaf_batch()``                             ``folio_pte_batch()``
``pgt_leaf_check_zapped()``                      ``arch_check_zapped_pte()``,
                                                 ``arch_check_zapped_pmd()``,
                                                 ``arch_check_zapped_pud()``
**Software entries**
``pgt_softleaf_mk()``                            ``softleaf_to_pte()``, ``softleaf_to_pmd()``
``softleaf_from_pgt()``                          ``softleaf_from_pte()``,
                                                 ``softleaf_from_pmd()``
``pgt_softleaf_soft_dirty()`` and its modifiers  ``pte_swp_soft_dirty()``,
                                                 ``pte_swp_mksoft_dirty()``,
                                                 ``pte_swp_clear_soft_dirty()``, PMD forms
``pgt_softleaf_uffd_wp()`` and its modifiers     ``pte_swp_uffd()``, ``pte_swp_mkuffd()``,
                                                 ``pte_swp_clear_uffd()``, PMD forms
``pgt_softleaf_batch()``                         ``swap_pte_batch()``
**Tables**
``pgt_table_mk()``                               none; ``pmd_populate()`` and kin build and
                                                 store in one step
``pgt_none_mk()``                                 none
**Locks**
``pgtp_lock_table()``                            ``pte_offset_map_lock()``
``pgtp_lock_entry()``                            ``pmd_lock()``, ``pud_lock()``
``pgtp_unlock()``                                ``pte_unmap_unlock()``, ``spin_unlock()``
``pgtp_map()``, ``pgtp_unmap()``                 ``pte_offset_map_ro_nolock()``,
                                                 ``pte_unmap()``
``pgtp_lockptr()``                               ``pte_lockptr()``, ``pmd_lockptr()``,
                                                 ``pud_lockptr()``
**TLB**
``tlb_remove_pgt_entry()``                       ``tlb_remove_tlb_entry()``,
                                                 ``tlb_remove_pmd_tlb_entry()``,
                                                 ``tlb_remove_pud_tlb_entry()``
``tlb_remove_pgt_entries()``                     ``tlb_remove_tlb_entries()``
``tlb_remove_pgt_pages()``                       ``__tlb_remove_folio_pages()``,
                                                 ``tlb_remove_page_size()``
``tlb_remove_pgt_table()``                       ``pte_free_tlb()``, ``pmd_free_tlb()``,
                                                 ``pud_free_tlb()``
===============================================  ==============================================

Notes on the mapping
--------------------

- ``pgt_leaf_t``, ``pgt_softleaf_t`` and ``pgt_table_t`` have no
  counterpart: the helpers leave the kind to the caller, and mixing up the
  present and the software form of an entry, ``pte_soft_dirty()`` against
  ``pte_swp_soft_dirty()``, is a recurring bug.
- ``pgt_leaf_mk()`` and ``pgt_leaf_modify()`` relocate the level-dependent
  protection bits at every level. The helpers are not consistent here:
  ``pmd_set_huge()`` relocates the x86 PAT bit, ``pfn_pmd()`` does not, and
  callers have to know which one they use.
- ``pgt_parent_references()``, the check ``pgtp_lock_table()`` makes, is
  the check ``pte_offset_map_lock()`` makes with ``pmd_same()``. Comparing
  against a snapshot gives the same answer on x86, where table entries are
  written with the accessed and dirty bits already set.
- The three places that take two page table locks today use three
  different ordering rules: ``copy_pte_range()``, ``move_ptes()`` and
  ``double_pt_lock()``.
- A ``pte_hole(depth)`` callback of ``walk_page_range()`` corresponds to a
  NONE that ``struct pgt_iter`` yields at that level.


Fallback
========

The fallback is the architecture contract written on top of the per-level
helpers, for every ``CONFIG_PGTABLE_LEVELS`` from 2 to 5. Every operation on an entry
does what the helper of that level does on the architecture:

- ``pgt_table_child()`` is ``pXd_offset()`` of the next level, applied to
  the snapshot;
- ``pgtp_addr_end()`` is ``pXd_addr_end()``;
- each kind test is the predicate of that level (see `Classification`_).

Folded levels are not visited. A level folded at build time is not a pgt
level at all. Levels folded at run time (x86-64 without 5-level paging,
riscv, arm64 with LPA2, s390) are always the topmost ones, and
``mm_p4d_folded()``, ``mm_pud_folded()`` and ``mm_pmd_folded()`` say which;
``pgt_root_level()`` is the highest level that is not folded, and
``pgtp_root()`` steps through the folded ones with ``pXd_offset()`` to the
entry at that level. Nothing below the root is folded.

The fallback therefore has the same levels as a native implementation on
the same machine: on x86-64, ``pgt_root_level()`` is 3 or 4 either way.

Classification
--------------

The fallback classifies with the predicates of the level:

1. ``pXd_none()``: NONE.
2. ``pXd_present()``: LEAF if ``pXd_leaf()``, else BAD if ``pXd_bad()``, else
   TABLE.
3. Not present: SOFTLEAF if the level can hold one; otherwise LEAF if
   ``pXd_leaf()``, else BAD.

At level 0 it is ``pte_none()``, ``pte_present()``, SOFTLEAF.

The leaf test comes before ``pXd_bad()`` because ``pmd_bad()`` is true for
a huge PMD on several architectures; legacy walkers have to test for a huge
PMD before ``pmd_bad()`` for the same reason. It comes after
``pXd_present()`` because ``pXd_leaf()`` is only defined for present
entries; on powerpc a software entry carries the leaf bit. The third step
keeps x86 PROT_NONE PUD leaves, which ``pud_present()`` rejects, out of BAD.

Limits
------

Where the helpers cannot express something, the fallback does what they
can:

- Leaves at levels 1 and 2 need ``CONFIG_TRANSPARENT_HUGEPAGE``, and level
  2 also ``CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD``, for their flag
  helpers; without them flags read as clear and modifiers do nothing.
- A flag with no helper at a level reads as clear: user, no-exec,
  contiguous, global and saved-dirty everywhere (``pte_dirty()`` already
  counts saved-dirty), soft-dirty above level 1. ``pgt_leaf_exec()`` is
  therefore true for every leaf on the fallback.
- ``pgt_leaf_mk()``, ``pgt_leaf_modify()`` and ``pgt_leaf_pgprot()`` above
  level 0 do what ``pfn_pmd()``, ``pmd_modify()`` and ``pmd_pgprot()`` do.
  On x86 that means no PAT relocation, and ``pmd_pgprot()`` returns the
  large-page layout, PSE included (see :doc:`pgt`, Canonical pgprot).
- ``pgt_none_mk()`` is zero. The helpers can test for the empty entry and
  clear one, but cannot name it; on s390 the empty entry is not zero.
- ``pgt_table_mk()`` warns: ``pXd_populate()`` builds and stores a table
  entry in one step, with side effects such as paravirt pinning. Exchanging
  in a table entry with ``pgtp_xchg()`` warns too: there is no common
  setter for a table entry (powerpc has no ``set_pmd()``). Tables are
  linked with ``pgt_alloc_child()``, which calls ``__pte_alloc()`` and its
  kin.
- The TLB macros expand to ``tlb_remove_tlb_entry()``,
  ``tlb_remove_pmd_tlb_entry()``, ``tlb_remove_pud_tlb_entry()`` and the
  ``pXd_free_tlb()`` helpers.

s390 root level
---------------

On s390 the root level of a live mm can grow. When an mm needs more address
space, s390 adds a level on top and updates ``mm->pgd`` and the address
space limit that ``mm_p4d_folded()`` and ``mm_pud_folded()`` read, one after
the other. A walker that holds neither mmap_lock nor the VMA lock (GUP-fast)
can see the new root with the old level. The s390 GUP-fast does not have the
problem: it decides folding from the type of each entry.


Disagreements
=============

Places where a per-level predicate and the kinds model disagree, and what
pgt does. They were found while writing the x86-64 instance.

x86: leaf before present at level 1
  ``pmd_present()`` is true for any PSE entry, so that a PMD between
  ``pmdp_invalidate()`` and the following store stays present. In pgt such
  an entry is a LEAF by the leaf test; ``pgt_leaf_accessible()`` is false.

x86: ``pud_present()`` ignores ``_PAGE_PROTNONE``
  ``pmd_present()`` and ``pte_present()`` accept it. A PSE entry at level 2
  is a LEAF in pgt either way.

x86: ``pud_bad()`` and ``p4d_bad()`` are looser than ``pmd_bad()``
  They accept any subset of the table flags, including one without
  ``_PAGE_PRESENT``; a legacy walk follows such an entry. The native table
  pattern is ``pmd_bad()``'s at every level and calls it BAD; at level 4 it
  also requires ``_PAGE_ACCESSED``, as ``pgd_bad()`` does. Only entries that
  the kernel never writes differ.

x86: the NONE test ignores accessed and dirty
  ``pte_none()``, ``pmd_none()``, ``pud_none()`` and ``p4d_none()`` mask
  ``_PAGE_KNL_ERRATUM_MASK``; ``pgd_none()`` does not. The x86-64 instance
  follows the helpers level by level.

x86: protection keys
  ``pte_access_permitted()`` checks PKRU as well as the entry bits, and so
  does ``pgt_leaf_accessible()``.


Rules
=====

- No new feature goes into a per-level helper family once pgt has the
  equivalent.
- The consumer API never calls the per-level helpers; the fallback does.


Deprecation path
================

The helper families stay until nothing in core mm needs them. The order:
walkers that only read (pagewalk, GUP-fast), then the ones that only clear
(zap), then the fault path, the two-tree walkers (fork, mremap,
``UFFDIO_MOVE``), hugetlb and kernel mappings. Each conversion adds what it
needs to the consumer API and nothing else. A helper is removed when it has no
users outside architecture code; an architecture helper is removed when its
architecture has a native implementation and nothing else calls it.
Nothing is marked deprecated in the meantime.
