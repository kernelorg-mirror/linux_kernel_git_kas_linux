.. SPDX-License-Identifier: GPL-2.0

=================================
Level-generic page table API: pgt
=================================

pgt is an API for walking and editing user page tables in which the page
table level is data. One entry pointer type and a small set of entry value
types carry the level, so an operation is written once for every level and
a walker is one function body that the compiler instantiates per level.

pgt has two sides:

The consumer API
  ``include/linux/pgt.h`` and ``include/linux/pgt-walk.h``, with the types
  alone in ``include/linux/pgt_types.h`` for headers that only embed or
  pass them. Generic code, built strictly on the architecture contract.

The architecture contract
  ``include/asm-generic/pgt-contract.h``: a table describing the entry
  format of each level and a short list of hooks with generic defaults.

Every architecture gets the contract from a fallback, so a converted
consumer runs on every architecture unchanged. An architecture may provide
a native implementation instead; x86-64 does. :doc:`pgt-legacy` describes
the fallback and how pgt relates to the per-level helpers it replaces.

pgt covers user page tables reached from ``mm->pgd``. Kernel mappings (see
`Kernel page tables`_), hugetlb and the fault path are not covered yet.


Levels and types
================

Levels are numbers. The leaf table is level 0, and level ``n + 1`` holds
the entries that link level-``n`` tables, up to ``PGT_MAX_LEVEL``, which is
``CONFIG_PGTABLE_LEVELS - 1``. On x86-64 an entry maps 4K at level 0, 2M at
level 1 and 1G at level 2. The level of the root table of an mm is a
runtime value, ``pgt_root_level(mm)``: 3 or 4 on x86-64 depending on
5-level paging.

``pgtp_t`` points to an entry and carries the level of the table it points
into, next to the address. ``PGTP_NULL`` is the null pointer; it is the
parent of a root entry.

``pgtv_t`` is a snapshot of an entry of any kind: the raw value and its
level. Values of a known kind have their own types (see `Typed values`_).
The raw value has the type ``pgtval_t``: ``unsigned long`` on 64-bit, ``u64`` on
32-bit so that PAE and LPAE entries fit, and ``u128`` on an architecture
with 128-bit entries (arm64 FEAT_D128), which selects
``CONFIG_PGT_128BIT``. Generic code never looks inside the raw value; every
question about it goes through the architecture contract.

A value always carries its level, also on architectures whose entry format
looks the same at every level, because the meaning of the bits changes. On
x86-64 bit 7 is PAT at level 0, PSE at levels 1 and 2 and reserved above;
bit 12 is a PFN bit at level 0 and the large-page PAT bit at levels 1 and 2.

The level is not part of the type, so storing a value at the wrong level is
not a compile error. pgt checks it at run time with ``CONFIG_DEBUG_VM``:

- the stores and ``pgtp_xchg()`` assert that the value and the pointer
  are at the same level;
- ``pgtv_same()`` asserts that both values are at the same level;
- ``pgtv_to_leaf()``, ``pgtv_to_softleaf()`` and ``pgtv_to_table()`` assert
  the kind of the value;
- ``pgt_leaf_mk()`` and ``pgt_softleaf_mk()`` assert that the level can
  hold the kind.


Kinds
=====

Every value is exactly one of five kinds:

``PGT_NONE``
  Empty. Nothing is mapped and no table is linked.

``PGT_TABLE``
  Links the table one level down.

``PGT_LEAF``
  Maps memory of the entry's size. "Leaf" means Linux-present, not
  hardware-valid: a ``PROT_NONE`` entry and an entry between
  ``pgtp_invalidate()`` and the store that follows it are leaves.
  ``pgt_leaf_accessible()`` answers whether the hardware can use the entry.

``PGT_SOFTLEAF``
  A software entry that maps nothing the hardware can see: swap,
  migration, device-private, markers. Its content is a ``softleaf_t``.

``PGT_BAD``
  None of the above. A walker reports or clears it; it never follows it.

``pgtv_kind()`` classifies one snapshot. It does not reload the entry
between tests, so a value that changes concurrently is still put in exactly
one kind. Reload only under the lock.

Typed values
------------

A value of a known kind has a type of its own: ``pgt_leaf_t``,
``pgt_softleaf_t`` or ``pgt_table_t``. Each wraps a ``pgtv_t``, and only the
operations of that kind take it, so reading a leaf bit from a software
entry, or following a leaf as if it linked a table, does not compile.

A walker reads a ``pgtv_t``, switches on its kind and converts::

  v = pgtp_get(p);
  switch (pgtv_kind(v)) {
  case PGT_TABLE:
          t = pgtv_to_table(v);
          child = pgt_table_child(t, addr);
          ...
  case PGT_LEAF:
          l = pgtv_to_leaf(v);
          pfn = pgt_leaf_pfn(l);
          ...
  case PGT_SOFTLEAF:
          s = pgtv_to_softleaf(v);
          e = softleaf_from_pgt(s);
          ...
  }

``pgtv_to_leaf()``, ``pgtv_to_softleaf()`` and ``pgtv_to_table()`` warn with
``CONFIG_DEBUG_VM`` when the value is of another kind; the conversion is
the one place where a wrong kind can still slip through.
``pgtv_from_leaf()``, ``pgtv_from_softleaf()`` and ``pgtv_from_table()`` go
back, for the operations that take any kind: ``pgtp_xchg()`` and
``pgtv_same()``. NONE and BAD have no type of their own: a NONE value
carries nothing but its level, and a BAD one is only ever cleared.

Generic classification
----------------------

The format table (see `Architecture contract`_) gives, per level, masks
for each test. Classification applies them in this order:

1. ``(val & none_mask) == none_val``: NONE.
2. At level 0: ``val & present_mask``: LEAF, otherwise SOFTLEAF. A level-0
   value is never TABLE or BAD.
3. The level can hold a leaf and ``(val & leaf_mask) == leaf_val``: LEAF.
4. ``!(val & present_mask)``: SOFTLEAF if the level can hold one, otherwise
   BAD.
5. ``(val & table_mask) != table_val`` or ``val & mbz``: BAD.
6. Otherwise TABLE.

The leaf test comes before the present and table tests at levels 1 and
above. A large leaf looks like a malformed table entry on most formats, and
an invalidated or ``PROT_NONE`` large leaf looks non-present.

x86-64 instance
---------------

=====  ===============================  =================  ========  ===========================
Level  NONE                             LEAF               SOFTLEAF  TABLE
=====  ===============================  =================  ========  ===========================
0      no bits outside accessed/dirty   present or         else      never
                                        PROT_NONE
1      no bits outside accessed/dirty   PSE                if non-   table pattern
                                                           present
2      no bits outside accessed/dirty   PSE                never     table pattern
3      no bits outside accessed/dirty   never              never     table pattern, NX ignored
4      zero                             never              never     table pattern, NX ignored
=====  ===============================  =================  ========  ===========================

- The accessed and dirty bits are ignored by the NONE test at levels 0 to 3
  (``_PAGE_KNL_ERRATUM_MASK``): Knights Landing can set them in a
  non-present entry. Level 4 does not ignore them.
- ``present_mask`` is ``_PAGE_PRESENT | _PAGE_PROTNONE`` at every level.
- The table pattern: the flag bits other than ``_PAGE_USER`` and
  ``_PAGE_ACCESSED`` equal ``_KERNPG_TABLE`` without ``_PAGE_ACCESSED``. At
  levels 3 and 4 ``_PAGE_NX`` is also ignored when page table isolation is
  built in, because it sets NX in the kernel copy of user top-level entries.
- ``_PAGE_PSE`` is in ``mbz`` at levels 3 and 4, so a PSE entry there is BAD.
- Level-1 migration and device-private entries exist (with
  ``CONFIG_ARCH_HAS_PMD_SOFTLEAVES``); level-2 software entries do not. A
  non-present, non-PSE entry at level 2 or above is BAD.


Entry operations
================

Pointers
--------

``pgtp_root(mm, addr)`` returns the root entry for ``addr``. All decisions
are made on snapshots, taken in one of two ways:

- ``pgtp_get(p)`` when the entry cannot change under the caller, normally
  because the caller holds its lock;
- ``pgtp_get_lockless(p)`` when it can: GUP-fast, walks under RCU only, and
  the entries a walker passes through above the level it locks.

The two differ where a plain read is not enough without the lock: 64-bit
entries read in two halves on 32-bit, and contiguous blocks whose accessed
and dirty bits have to be gathered consistently (arm64). Under the lock the
cheaper read is correct; elsewhere only the lockless one is.

``pgt_table_child(t, addr)`` returns the entry for ``addr`` in the table
that ``t`` links. The table address comes from ``t``; the entry ``t`` was
read from is not read again.

``pgtp_next(p)`` and ``pgtp_advance(p, n)`` step within a table.
``pgtp_addr_end(p, addr, end)`` returns the end of the entry's range
clipped to ``end``. Above level 0 ``addr`` and ``end`` may be any address,
as a walk of an unaligned range has them. At level 0 they are page
aligned, so the entry never passes ``end`` and its end is
``addr + PAGE_SIZE``.
``pgtp_table(p)`` returns the ``struct ptdesc`` of the table that contains
``*p``.

Stores
------

There is one store per kind, because architectures store each kind
through a different path: ``pgtp_set_leaf()`` and ``pgtp_set_leaf_nr()``
store leaves, ``pgtp_set_softleaf()`` a software entry, ``pgtp_clear()``
empties an entry, and tables are linked with ``pgt_alloc_child()`` below.
``pgtp_xchg()`` takes a value of any kind and returns the old one. All of
them take the mm and the address because architectures need them (PTI
mirroring of the top level, page table check, hypervisor hooks).
``pgtp_set_leaf_nr()`` stores ``nr`` consecutive leaves mapping
consecutive memory; ``nr`` above one is only valid at level 0.

``pgtp_invalidate()`` is defined above level 0: the entry stays a LEAF the
hardware can no longer use, the TLB is flushed for its range, and the old
leaf is returned with every accessed and dirty bit the hardware set until
then. Level 0 has no split to protect, and clearing the entry is enough
there.

``pgt_alloc_child(mm, p, &v)`` allocates the table one level below ``p``,
takes the lock that protects ``*p``, rechecks that ``*p`` is still NONE and
links the table or frees it. On return ``v`` is a fresh snapshot of ``*p``.

``pgtp_clear_bad(p)`` reports and clears a BAD entry.

``tlb_remove_pgt_entry(tlb, p, addr)`` records a cleared leaf for the TLB
flush. ``tlb_remove_pgt_table(tlb, t, addr)`` frees, after the flush, the
table that the table entry ``t`` linked before the entry was cleared, and
drops the mm's accounting for it. Both are macros, since they expand to the
``<asm/tlb.h>`` helpers.

Values
------

Leaf predicates (``pgt_leaf_write()``, ``pgt_leaf_dirty()``,
``pgt_leaf_young()``, ``pgt_leaf_special()``, ``pgt_leaf_exec()``,
``pgt_leaf_user()``, ``pgt_leaf_cont()``) and modifiers
(``pgt_leaf_mkwrite()``, ``pgt_leaf_wrprotect()``, ``pgt_leaf_mkdirty()``,
``pgt_leaf_mkclean()``, ``pgt_leaf_mkyoung()``, ``pgt_leaf_mkold()``,
``pgt_leaf_mkspecial()``) take a leaf. Soft-dirty and uffd-wp exist on
leaves and on software entries, in different places, so each has a
``pgt_leaf_*()`` and a ``pgt_softleaf_*()`` form; the format table says
where the bits are. ``pgt_leaf_dirty()`` is true for a hardware-dirty and
a saved-dirty entry (x86 shadow stack). ``pgt_leaf_mkdirty()`` also sets
soft-dirty.

``pgt_leaf_modify(l, prot)`` takes the access permissions from ``prot`` and
keeps the PFN and the young, dirty, soft-dirty, special and uffd-wp state.
The memory type is kept on x86 and taken from ``prot`` on arm64, riscv and
powerpc.

``pgt_leaf_pfn()``, ``pgt_leaf_shift()`` and ``pgt_leaf_size()`` describe
the memory a leaf maps. ``pgt_leaf_next(l)`` advances the PFN by the size
of the leaf. ``pgt_leaf_batch()`` counts the level-0 entries from a pointer
that map consecutive pages of one folio and differ from the first only in
the PFN and the accessed, writable, dirty and soft-dirty state.

``pgtv_same(a, b)`` compares two values of any kind at the same level as
``pte_same()`` does: bit for bit, except for bits that only the
architecture's own bookkeeping changes, such as the hash bits on powerpc.
``pgtp_size(p)`` is the size of the range an entry covers.

``pgt_none_mk(lvl)`` builds a NONE value and ``pgt_table_mk(table, lvl)`` a
table entry linking ``table``.

Canonical pgprot
----------------

A ``pgprot_t`` crossing the consumer API is always in the level-0 layout.
``pgt_leaf_mk(pfn, prot, lvl)`` relocates bits whose position depends on
the level, and ``pgt_leaf_pgprot(l)`` moves them back; ``pgt_leaf_modify()``
relocates the same way. On x86 that is the PAT bit: bit 7 at level 0, bit
12 at levels 1 and 2. The format table does not describe such bits; an
architecture that has them overrides ``pgt_arch_mk()``,
``pgt_arch_pgprot()`` and ``pgt_arch_modify()``.

Software entries
----------------

``softleaf_from_pgt(s)`` and ``pgt_softleaf_mk(e, lvl)`` convert between a
``pgt_softleaf_t`` and ``softleaf_t``.


Locking and lifetime
====================

Lock protocol
-------------

A level-0 table can be freed while a walker looks at it (see `Lifetime`_),
so taking its lock can fail. A failure means "the table changed under you,
walk again from this address"; it never means "skip this range". A table
above level 0 cannot go away under the caller, and the lock of an entry
there is taken directly.

``pgtp_lock_table()`` and ``pgtp_map()`` take the pointer ``p`` and the
parent pointer ``pp``, the entry ``p`` was reached through. ``pp`` is
``PGTP_NULL`` for a root entry.
::

  pgtp_lock_table(mm, p, pp):         level 0, locks the whole table
      rcu_read_lock()
      if (pp && !pgt_parent_references(pp, p))
          rcu_read_unlock(); return NULL
      ptl = pgtp_lockptr(mm, p); spin_lock(ptl)
      if (pp && !pgt_parent_references(pp, p))
          spin_unlock(ptl); rcu_read_unlock(); return NULL
      return ptl

  pgtp_lock_entry(mm, p):             levels >= 1, locks the entry
      ptl = pgtp_lockptr(mm, p); spin_lock(ptl)
      return ptl

  pgtp_unlock(p, ptl):                spin_unlock(ptl); at level 0 also
                                      rcu_read_unlock()

  pgtp_map(p, pp):                    read-only access, no lock
      rcu_read_lock()
      if (pp && !pgt_parent_references(pp, p))
          rcu_read_unlock(); return false
      return true

  pgtp_unmap(p):                      rcu_read_unlock()

``pgt_parent_references(pp, p)`` reads ``*pp`` with ``pgtp_get_lockless()``
and checks that it is a TABLE linking the table that contains ``p``.

The first check, before the lock is taken, is what makes it safe to compute
``p`` without holding RCU: ``pgtp_lockptr()`` reads the ``struct ptdesc`` of
the table, and that table may have been freed between the snapshot of
``*pp`` and ``rcu_read_lock()``. Once ``*pp`` is seen linking the table
inside the RCU section, the table cannot be freed before
``rcu_read_unlock()``. The second check, under the lock, is the one that
makes the locked table the live one.

Which lock: for a level in the format table's ``split_lock`` mask, the
split lock in the ``struct ptdesc`` of the table that contains ``p``;
otherwise ``mm->page_table_lock``. At level 0 the lock covers the whole
table and is normally held while the walker processes it. Above level 0 it
covers one entry.

With ``CONFIG_HIGHPTE`` a level-0 table may have no permanent mapping.
``pgt_table_child()`` at level 1 maps it with ``kmap_local_page()``;
``pgtp_unlock()`` and ``pgtp_unmap()`` unmap it, and so do
``pgtp_lock_table()`` and ``pgtp_map()`` when they fail. Every level-0
pointer from ``pgt_table_child()`` must be passed to one of those two
before it is used.

Two-table locking (``pgtp_lock2()``) is not part of this interface yet. It
comes with the conversion of fork, mremap and ``UFFDIO_MOVE``, which take
two page table locks today with three different ordering rules; that is
the reason it has to be a single primitive.

Lifetime
--------

A ``pgtp_t`` into a user page table stays valid while one of these holds:

- RCU is held and the table was seen linked under it. User page tables are
  freed through ``mmu_gather`` after an RCU grace period on architectures
  with ``CONFIG_MMU_GATHER_RCU_TABLE_FREE``, and collapsed level-0 tables
  are freed the same way.
- Interrupts are disabled, on architectures that free tables only after an
  IPI that this CPU must answer (GUP-fast).
- The table is pinned: mmap_lock or the VMA lock is held for read and the
  lock of the parent entry is held.

Tables at level 1 and above are freed by ``free_pgtables()`` only, after the
VMAs that cover them are detached and unlinked from rmap, so mmap_lock or
the VMA lock held for read keeps them, and so does the rmap lock a VMA was
found through. Level-0 tables can be freed under mmap_lock held for read
(empty table reclaim after ``MADV_DONTNEED``, khugepaged collapse). They
are therefore used only between ``pgtp_lock_table()`` and ``pgtp_unlock()``
or between ``pgtp_map()`` and ``pgtp_unmap()``.

What the caller holds, per function:

- ``pgtp_root()``: a reference on the mm.
- ``pgtp_get_lockless()``, ``pgt_table_child()``, ``pgtp_next()``,
  ``pgtp_advance()``, ``pgtp_table()``: one of the conditions above for the
  table containing the entry.
- ``pgtp_get()``: the lock of ``p``, or another guarantee that ``*p`` does
  not change.
- ``pgtp_set_leaf()``, ``pgtp_set_leaf_nr()``, ``pgtp_set_softleaf()``,
  ``pgtp_xchg()``, ``pgtp_clear()``, ``pgtp_invalidate()``: the lock of
  ``p``.
- ``pgtp_clear_bad()``: what ``pgtp_get_lockless()`` needs, as with
  ``pmd_clear_bad()``. Nothing else writes a BAD entry, so nothing races
  with clearing it.
- ``pgtp_lock_table()``, ``pgtp_map()``: mmap_lock or the VMA lock for
  read, the rmap lock the VMA was found through, or interrupts disabled;
  RCU is taken inside.
- ``pgtp_lock_entry()``: mmap_lock or the VMA lock for read, or the rmap
  lock the VMA was found through.
- ``pgt_alloc_child()``: mmap_lock or the VMA lock for read; it may sleep.
- ``pgt_iter_*()``: depends on the flags, see `Iterator`_.


Walkers
=======

Level walker
------------

A walker is one function body that handles any level, instantiated once per
level::

  static __always_inline unsigned long
  body(ctx_t ctx, pgtp_t p, pgtp_t pp, unsigned long addr,
       unsigned long end, const unsigned int lvl);

  PGT_DEFINE_LEVEL_WALKER(name, body, ctx_t);

The macro declares ``body``, emits ``name_l0()`` to ``name_lN()`` that call
it with a constant ``lvl``, and an ``__always_inline`` dispatcher
``name(ctx, p, pp, addr, end, lvl)`` whose ``switch (lvl)`` folds to a
direct call when ``lvl`` is a constant. The macro may come before the
definition of ``body``, so the body descends by calling ``name()`` with
``lvl - 1`` and each instance calls the next one directly.

A constant ``lvl`` alone does not help the operations, which read the
level from the entry pointer. Each instance therefore starts with
``pgtp_assume_level(p, lvl)``, which tells the compiler that the two
agree; ``pgtp_next()`` copies the level, so one statement covers the
whole loop. The level-0 instance also states that ``pp`` is at level 1.
The operations then compile for that level alone, as a loop written for
one level would. A walker written by hand does the same.

The claim only reaches code the compiler inlines. Every operation in
``<linux/pgt.h>`` and in the providers is ``__always_inline`` for that
reason: the inliner weighs a function before it knows the level, and an
operation that handles every level looks too big to inline. A helper of
the walker that takes an entry pointer and is not inlined starts with its
own ``pgtp_assume_level()``.

The return value is the address the walk reached. ``end`` means the range
is done. Anything else means the body stopped there: a table went away
under it, it recorded an error in ``ctx``, or it gave up. What follows is
the consumer's choice. The zap walker retries the parent entry at that
address, the page walker returns the error it recorded, and GUP-fast hands
the rest of the range to the slow path. A consumer that restarts from the
root on every early stop must make sure the body cannot fail at the same
address forever.

The macro is a convenience. Code that needs a different structure uses the
pointer, value and lock functions directly.

Iterator
--------

``struct pgt_iter`` is the flattened form for a consumer that wants to own
the loop::

  PGT_ITER(it, mm, start, end, flags);

  for (v = pgt_iter_first(&it); !pgt_iter_done(&it); v = pgt_iter_next(&it)) {
          ...
  }

The iterator descends through TABLE entries by itself and yields every
other entry at the level where it finds it: a LEAF, SOFTLEAF, NONE or BAD.
``pgt_iter_addr()`` is the address of the yielded entry, clipped to the
range; ``pgt_iter_size()`` is the span from there to the end of the entry,
clipped to ``end``. A NONE at level 2 is a hole of up to the size of a
level-2 entry. ``pgt_iter_ptp()`` is the pointer to the yielded entry.

``PGT_ITER_RO`` walks under RCU without page table locks. With
``PGT_ITER_LOCKED`` the iterator holds the level-0 table lock while it
yields entries of that table, and drops it when it moves to the next
table; a lock failure makes it walk again from the current address.
A consumer that leaves the loop before ``pgt_iter_done()`` calls
``pgt_iter_stop()`` to release what the iterator holds.


Architecture contract
=====================

Format table
------------

The architecture describes each level in a ``struct pgt_level_fmt``:

- ``shift``, ``entry_order`` and ``entries``: the size an entry maps, the
  size of an entry, and the number of entries in a table;
- ``none_mask``/``none_val``, ``leaf_mask``/``leaf_val``, ``present_mask``,
  ``table_mask``/``table_val`` and ``mbz``: the classification masks; a
  zero ``leaf_mask`` means the level cannot hold a leaf;
- ``table_flags``: the bits a table entry carries next to the table address;
- ``table_pfn_mask`` and ``leaf_pfn_mask``: the PFN field of a table
  entry and of a leaf, zero at a level that holds no table or no leaf;
  ``pfn_shift``, and ``pfn_hi_mask``/``pfn_hi_shift`` for formats that
  split the physical address;
- ``bit[PGT_NR_FLAGS]``: the position of each software-visible flag at this
  level, zero when the level does not have it;
- ``cont_entries``: the length of a contiguous-hint run, zero if none.

``struct pgt_fmt`` collects the levels and adds the per-level capability
masks ``can_leaf``, ``can_softleaf`` and ``split_lock``, and the layout of
a software entry (type and offset fields, whether the offset is stored
inverted, and the bits that must be clear in one).

Everything derivable from these tables is implemented once, generically:
kinds, PFN, ``pgt_leaf_mk()``, flag predicates and modifiers,
``pgt_table_mk()``, ``pgt_none_mk()``, softleaf conversion and BAD
detection. With a constant level the table loads fold to constants when the
architecture provides the table as ``static const``; with a run-time level
they are plain loads with no branches.

Each of these derived operations is itself a ``pgt_arch_*()`` entry point
whose default reads the table: ``pgt_arch_kind()``, ``pgt_arch_pfn()``,
``pgt_arch_mk()``, ``pgt_arch_modify()``, ``pgt_arch_pgprot()``,
``pgt_arch_test_flag()``, ``pgt_arch_set_flag()``, ``pgt_arch_clear_flag()``,
``pgt_arch_next()``, ``pgt_arch_same()``, ``pgt_arch_none_mk()``,
``pgt_arch_table_entry()``, the softleaf conversions, ``pgt_arch_shift()``,
``pgt_arch_entry_order()``, ``pgt_arch_can_leaf()``,
``pgt_arch_can_softleaf()``, ``pgt_arch_root()``, ``pgt_arch_child()``,
``pgt_arch_addr_end()``, ``pgt_arch_links()``, ``pgt_arch_lockptr()``,
``pgt_arch_clear()``, ``pgt_arch_clear_bad()``, ``pgt_arch_alloc_child()``
and ``pgt_arch_leaf_batch()``. A native provider normally leaves them
alone; the fallback, which has no table, overrides every one.

Some fields are known only at boot on some configurations: on x86 the
memory encryption bit in table entries and the physical address mask with
``CONFIG_X86_MEM_ENCRYPT``. The generic code reads ``table_pfn_mask``,
``leaf_pfn_mask``, ``table_mask``, ``table_val`` and ``table_flags``
through accessors, ``pgt_fmt_table_pfn_mask()`` and so on, and the
architecture overrides the accessor of such a field; the rest of the table
still folds.

Hooks
-----

Behaviour the tables cannot express is a hook. Each has a generic default,
and an architecture overrides it with the usual pattern::

  #define pgt_arch_store_leaf pgt_arch_store_leaf
  static inline void pgt_arch_store_leaf(...)

==============================  ======================================  ===========================
Hook                            Default                                 Overridden for
==============================  ======================================  ===========================
``pgt_arch_load()``             ``READ_ONCE()`` of the entry width      paravirt (x86)
``pgt_arch_load_lockless()``    ``pgt_arch_load()``                     halves (32-bit), contiguous
                                                                        blocks (arm64)
``pgt_arch_store_leaf()``       plain store of ``nr`` entries           paravirt, PTI mirror (x86)
``pgt_arch_store_softleaf()``   plain store                             paravirt (x86)
``pgt_arch_xchg()``             ``xchg()``                              paravirt (x86)
``pgt_arch_invalidate()``       exchange with the invalid encoding,     own invalidate sequence
                                flush the range
``pgt_arch_populate()``         store ``pgt_table_mk()``                Xen PV table pinning (x86)
``pgt_arch_root_table()``       ``mm->pgd``                             PTI, TTBR, ASCE selection
``pgt_arch_root_level()``       ``PGT_MAX_LEVEL``                       5-level paging (x86)
``pgt_arch_mkwrite()``          set ``bit[WRITE]``, move saved-dirty    shadow stack (x86), DBM
                                to dirty                                (arm64)
``pgt_arch_mkdirty()``          from ``bit[]``                          shadow stack (x86)
``pgt_arch_wrprotect()``        from ``bit[]``                          shadow stack (x86)
``pgt_arch_write()``            ``bit[WRITE]``                          shadow stack (x86)
``pgt_arch_accessible()``       valid, user and, for a write,           protection keys (x86)
                                ``pgt_arch_write()``
``pgt_arch_pfn_inverted()``     false                                   L1TF (x86)
``pgt_arch_table_alloc()``      ``pagetable_alloc()``, filled with      the root table (x86)
                                ``none_val``
``pgt_arch_table_free()``       ``pagetable_free()``
``pgt_arch_table_ctor()``       the ptdesc constructors
``pgt_arch_table_dtor()``       ``pagetable_dtor()``
``pgt_arch_deposit()``          the generic deposit list                ppc hash, s390
``pgt_arch_withdraw()``         the generic deposit list                ppc hash, s390
==============================  ======================================  ===========================

TLB and cache maintenance stay with the existing hooks:
``flush_tlb_range()``, ``update_mmu_cache_range()``, ``tlb_remove_table()``
and ``arch_sync_kernel_mappings()``. Recording a cleared leaf and freeing
an unlinked table for ``mmu_gather`` are the macros
``pgt_arch_tlb_remove_entry()`` and ``pgt_arch_tlb_remove_table()``, with
no generic default.

A coupled bit rule, where the meaning of one bit depends on another, is a
hook and not a table entry: the format table has no interpreter.
``pgt_arch_write()``, ``pgt_arch_accessible()`` and ``pgt_arch_pfn_inverted()``
exist because of x86 rules of this kind (see `x86-64 format rules`_).

x86-64 overrides
----------------

``load``, the stores and ``xchg`` go through the paravirt page table
operations, so ``CONFIG_PARAVIRT_XXL`` kernels and Xen PV keep working. A
store at the root level also updates the PTI user copy of the top-level
table. ``populate`` tells the hypervisor about the new table before it is
linked. ``mkwrite``, ``mkdirty``, ``wrprotect`` and ``write`` implement the
shadow stack rules around ``_PAGE_SAVED_DIRTY``. ``mk``, ``pgprot`` and
``modify`` move the PAT bit. ``accessible`` checks protection keys.
``pfn_inverted`` implements the L1TF rule. ``root_level`` follows
``pgtable_l5_enabled()``. The root table is not allocated through pgt.

x86-64 format rules
-------------------

PFN inversion
  A non-present, non-zero entry stores its PFN inverted (L1TF), so the PFN
  of a ``PROT_NONE`` or invalidated leaf cannot be read with
  ``leaf_pfn_mask`` alone. ``pgt_arch_pfn_inverted()`` tells the generic
  code to invert; ``pgt_leaf_pfn()``, ``pgt_leaf_mk()``,
  ``pgt_leaf_modify()`` and ``pgt_leaf_next()`` apply it.

Shadow stack write and dirty
  An entry with ``_PAGE_RW`` clear and ``_PAGE_DIRTY`` set is a shadow stack
  entry and counts as writable. ``pgt_leaf_wrprotect()`` and
  ``pgt_leaf_mkdirty()`` move the dirty state to ``_PAGE_SAVED_DIRTY`` to
  avoid creating one by accident, and ``pgt_leaf_mkwrite()`` moves it back.

Protection keys
  Whether user access is allowed depends on PKRU as well as on the entry
  bits.

Boot-time masks
  ``_PAGE_ENC`` and the physical address mask are boot-time values with
  ``CONFIG_X86_MEM_ENCRYPT``, and the protection of a new leaf is filtered
  through ``__supported_pte_mask``.

PAT position
  See `Canonical pgprot`_.


Fallback
========

Architectures without a native implementation use
``<asm-generic/pgt-fallback.h>``, selected by ``CONFIG_PGT_FALLBACK``. It
has the same levels as a native implementation on the same machine, but
cannot express everything a format table can; :doc:`pgt-legacy` describes
how it works and where it falls short.

``CONFIG_ARCH_HAS_PGT`` is selected by architectures with a native
implementation.


Kernel page tables
==================

The same types, kinds and operations describe kernel page tables, reached
from ``init_mm``, and a walk that only reads them can use pgt as it is.
What differs is everything around the entries:

- Locking and lifetime. vmalloc and ioremap own their address ranges and
  store entries without a page table lock, and kernel tables are freed in a
  few places with their own synchronization: when a large vmalloc or
  ioremap mapping replaces a table, and on memory hot-remove. The lock
  protocol above does not apply, and kernel walkers need rules of their
  own.
- Allocation. Kernel tables come from ``GFP_PGTABLE_KERNEL``, without memory
  cgroup accounting, and a new top-level entry reaches the other page
  tables through ``arch_sync_kernel_mappings()``.
- TLB. There is no ``mmu_gather``; changes are flushed with
  ``flush_tlb_kernel_range()``.
- Leaves. The direct map and large vmalloc mappings have leaves at levels 1
  and 2 whatever the THP configuration, and kernel callers set the memory
  type of a leaf, which ``pgt_leaf_modify()`` leaves to the architecture.

A read-only walker, such as the page table dump, is the natural first
kernel user.


Rules
=====

- Protocols that belong to an operation stay with its caller:
  ``mmu_notifier`` calls, ``mmu_gather`` setup, the placement of
  ``flush_tlb_batched_pending()``, ``write_protect_seq``.
- Generic code never names an architecture and never reads ``pgtv_t.val``.
