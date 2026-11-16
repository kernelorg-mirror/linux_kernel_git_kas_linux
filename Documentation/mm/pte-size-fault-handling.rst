.. SPDX-License-Identifier: GPL-2.0

===========================================
Page Fault Handling with PTE_SIZE < PG_SIZE
===========================================

This document enumerates every page fault path and what must change
when PG_SIZE > PTE_SIZE (i.e. one folio spans multiple PTEs).

Terminology
===========

- **PTE_SIZE**: size of the region one PTE maps (always 4K on x86)
- **PG_SIZE**: size of a base page / order-0 folio
  (buddy allocator granularity; may be 16K, 64K, ...)
- **PTES_PER_PAGE**: ``PG_SIZE / PTE_SIZE`` (1 when PG_SIZE == PTE_SIZE)
- **folio_nr_ptes(folio)**: how many PTEs a folio requires
  (``PTES_PER_PAGE`` for order-0)
- **pteoff**: offset in PTE_SIZE units (used in vm_fault, VMA, page cache)
- **pgoff / pgidx**: offset in PG_SIZE units
  (used by xarray / page cache indices)

Case inventory
==============

1. Anonymous first fault
------------------------

:Entry: ``do_anonymous_page()``
:Trigger: ``pte_none`` on anonymous VMA (``MAP_ANONYMOUS``)
:Folio: freshly allocated order-0 folio (PG_SIZE)

What happens
  Allocate one folio, install PTES_PER_PAGE PTEs pointing to successive
  PTE_SIZE chunks of it.

PTE_SIZE work
  - Compute ``folio_pteoff = round_down(vmf->pteoff, folio_nr_ptes)``.
  - Map ``nr_ptes = folio_nr_ptes(folio)`` PTEs starting at
    ``folio_pteoff``, provided the range fits entirely inside the VMA
    (``folio_pteoff >= vm_pteoff``,
    ``folio_pteoff + folio_nr_ptes <= vm_pteoff + vma_ptes(vma)``).
    Otherwise fall back to ``nr_ptes = 1``.
  - Clamp to PMD / page-table boundary
    (``BUG_ON(addr / PMD_SIZE != vmf->address / PMD_SIZE)``).
  - If the target PTE range is partially populated (another CPU faulted
    a sibling), retry with ``nr_ptes = 1``.

Sub-variant — read fault
  Read access maps the global zero page PTE_SIZE-granular (single PTE).
  No multi-PTE expansion — each sub-page gets its own zero PTE.  COW
  happens later per-PTE when written.

2. File read / read fault-around
--------------------------------

:Entry: ``do_read_fault()`` → ``finish_fault()`` (non-COW),
        ``do_fault_around()`` → ``filemap_map_pages()``
:Trigger: ``pte_none`` on file-backed VMA, read access
:Folio: page-cache folio (PG_SIZE or larger)

What happens
  Look up / read the folio from the page cache, map its PTEs.

PTE_SIZE work
  - ``finish_fault()``: compute folio sub-index
    ``idx = folio_page_idx(folio, page) * PTES_PER_PAGE +
    (vmf->pteoff % PTES_PER_PAGE)`` and map ``nr_ptes =
    folio_nr_ptes(folio)`` PTEs, clamped to VMA and page table.
  - ``do_fault_around()``: iterate in pteoff units.
    ``fault_around_pages`` is multiplied by PTES_PER_PAGE to yield the
    PTE range; from_pte/to_pte are clamped to VMA and PT boundaries.
  - ``filemap_map_pages()`` walks the page cache (PG_SIZE indices).
    ``vmf->pte`` advances by PTES_PER_PAGE per page-cache page.
  - ``filemap_map_folio_range()`` must map the sub-range of PTEs inside
    each folio that intersects ``[start_pteoff, end_pteoff]``.

3. File COW — first write (pte_none → write)
--------------------------------------------

:Entry: ``do_cow_fault()`` → ``finish_fault()`` with ``is_cow=true``
:Trigger: ``pte_none`` on ``MAP_PRIVATE`` file VMA, write access
:Folio: freshly allocated anonymous folio (PG_SIZE), data copied from
        the page cache

What happens
  Allocate a new anon folio, copy PG_SIZE bytes from the page-cache
  page, install PTES_PER_PAGE PTEs pointing to the new folio.

PTE_SIZE work
  - ``nr_ptes = PTES_PER_PAGE``, clamped to VMA and PT.
  - Sibling PTEs may already be:

    - ``pte_none``: install new PTE.
    - file-backed (from prior read fault / fault-around):
      ``ptep_clear_flush`` the old PTE, drop the rmap of the file folio,
      adjust MM_FILEPAGES → MM_ANONPAGES counter, install new PTE.
    - anonymous (prior sibling COW): fall back — siblings hold
      independent data we cannot merge into one copy.  Map only the
      faulting PTE.

  - If a sibling is anonymous, the copy source is that sibling's page
    (not the page cache page) so prior writes are preserved.

4. File COW — WP fault on read-mapped file page
-----------------------------------------------

:Entry: ``do_wp_page()`` → ``wp_page_copy()``
:Trigger: write to a read-only PTE in ``MAP_PRIVATE`` file VMA (PTE was
          installed by a prior read fault or fault-around)
:Folio: freshly allocated anonymous folio (PG_SIZE)

What happens
  Same as case 3 but starts from a present-but-RO file-backed PTE.

PTE_SIZE work
  - ``cow_find_sibling_anon()``: scan sibling PTEs in the same PG_SIZE
    group for an existing anon folio.  If found, copy from that folio
    (not the original file page) to preserve prior writes.
    Scan is lockless (advisory); needs a safe ``folio_try_get`` + RCU.
  - After installing the primary PTE under PTL,
    ``cow_install_sibling_ptes()`` replaces sibling PTEs that are
    pte_none or file-backed with mappings into the new anon folio.
    Anon siblings (from prior COW) are left untouched — their data is
    already in the new folio via the copy-from-sibling path.

5. Anonymous COW — fork + write
-------------------------------

:Entry: ``do_wp_page()`` → ``wp_page_copy()``
:Trigger: write to a COW-shared anonymous PTE after fork
:Folio: freshly allocated anon folio (PG_SIZE)

What happens
  Each PTE is independently COW-protected.  On write, allocate a new
  folio, copy from the parent's folio, replace the one faulting PTE.

PTE_SIZE work
  - **None for sibling PTEs.**  Anon COW siblings have independent data
    (parent may have written different values to different sub-pages
    before fork, or child writes different sub-pages at different
    times).  Each sibling COWs independently.
  - ``cow_install_sibling_ptes()`` is NOT called for anon VMAs
    (gated by ``vma->vm_file``).
  - The primary PTE still uses ``folio_mkpte(folio, pteoff, prot)`` to
    pick the correct sub-page offset within the new folio.

6. Swap-in
----------

:Entry: ``do_swap_page()``
:Trigger: PTE has swap entry (page was evicted)
:Folio: folio read back from swap (PG_SIZE)

What happens
  Read the swap slot, install PTE.

PTE_SIZE work
  - Currently: single-PTE swap-in.  Siblings remain as swap entries and
    fault individually (functional but suboptimal — one page-in per
    PTE instead of per PG_SIZE).
  - Future optimization: detect when all PTES_PER_PAGE siblings are
    swap entries pointing to contiguous slots and batch the swap-in.
  - ``folio_mkpte(folio, pteoff, prot)`` picks the correct sub-page.

7. Shared write fault
---------------------

:Entry: ``do_shared_fault()`` → ``finish_fault()``
:Trigger: write to ``MAP_SHARED`` file VMA
:Folio: page-cache folio (PG_SIZE or larger)

What happens
  Notify filesystem via ``vm_ops->page_mkwrite``, then map folio PTEs
  with write permission.

PTE_SIZE work
  - Same as case 2 (non-COW ``finish_fault``).  No copy, no sibling
    coordination.  All PTES_PER_PAGE PTEs map to the page-cache folio.

8. NUMA balancing fault
-----------------------

:Entry: ``do_numa_page()``
:Trigger: PTE marked ``_PAGE_PROTNONE`` for NUMA balancing
:Folio: existing page, possibly to be migrated

What happens
  Check if the page should migrate, reinstall PTE with correct
  protection.

PTE_SIZE work
  - Operates on a single PTE.  Functional as-is.
  - Future optimization: batch a whole folio's PTES_PER_PAGE siblings.

9. Special mapping fault (VDSO, vvar, uprobe xol)
-------------------------------------------------

:Entry: ``special_mapping_fault()``
:Trigger: ``pte_none`` on a ``vm_special_mapping`` VMA
:Folio: a ``struct page`` from the ``sm->pages[]`` array

What happens
  Return the struct page; generic fault path installs a single PTE.

PTE_SIZE work
  - Page index lookup uses ``vmf->pteoff / PTES_PER_PAGE`` to map
    PTE-offset space to the caller's PG_SIZE-indexed ``pages[]`` array.
  - Only one PTE is ever installed per call (siblings fault
    separately).  That's fine — the special mapping vs PTE_SIZE
    relationship is well-defined (one pages[] entry = one PG_SIZE page).

Summary table
=============

===========================  ================  ==================  =================================
Case                         Folio             Siblings             Required sibling handling
===========================  ================  ==================  =================================
1. Anon first fault          new anon          all pte_none         Map all PTES_PER_PAGE at once
2. File read                 cache folio       mix                  ``finish_fault`` clamps/maps folio
3. File COW (pte_none → W)   new anon          mix                  Replace file PTEs; fall back if
                                                                    anon sibling present
4. File COW (WP on RO file)  new anon          mix                  ``cow_install_sibling_ptes``
5. Anon COW (fork)           new anon          other anon           Leave siblings alone
6. Swap-in                   swap folio        swap entries         Single PTE (suboptimal)
7. Shared write              cache folio       mix                  ``finish_fault``, no copy
8. NUMA balancing            existing          N/A                  Single PTE
9. Special mapping           ``sm->pages[i]``  N/A                  Single PTE
===========================  ================  ==================  =================================

Key invariants
==============

- ``vmf->pteoff`` is the PTE offset; divide by ``PTES_PER_PAGE`` for
  page-cache (xarray) index.
- ``folio_mkpte(folio, pteoff, prot)`` constructs a PTE for the correct
  sub-page of ``folio`` based on ``pteoff``.
- Anonymous VMAs always have ``vm_pteoff = vm_start >> PTE_SHIFT``
  (PG_SIZE-aligned, because ``get_unmapped_area`` enforces PG_SIZE
  alignment).  Exception: ``mremap(MREMAP_FIXED)`` to a non-PG-aligned
  address leaves ``vm_pteoff`` tied to the original address — watch
  for this misalignment.
- File-backed VMAs inherit ``vm_pteoff`` from the syscall's ``offset``
  argument (via ``off >> PTE_SHIFT``).  Users can specify
  PTE-aligned-but-not-PG-aligned offsets, producing misaligned
  ``vm_pteoff`` — the fault handlers must handle that.
- Sibling coordination (cases 3, 4) only applies to **file-backed
  COW**, because only there can siblings be in different states
  (file-backed, none, or anon) simultaneously.

Gotchas
=======

- COW sibling PTE replacement must remove the old folio's rmap and
  drop its counter BEFORE ``folio_put()`` so that ``mm_counter_file()``
  doesn't dereference a freed folio (if the put was the last ref).
- ``folio_add_new_anon_rmap()`` sets mapcount to 1 for non-large
  folios.  When mapping ``nr_ptes > 1`` PTEs to the same order-0 folio,
  the caller must ``atomic_add(nr_ptes - 1, &folio->_mapcount)``.
  Direct mapcount manipulation should eventually be replaced by an
  rmap API that accepts ``nr_ptes``.
- ``cow_find_sibling_anon()`` scans PTEs without the PTL and does
  ``folio_get()`` on a folio it locates via ``pte_page()``.  Between
  the PTE read and the get, the folio can be freed.  Needs
  ``folio_try_get()`` inside an RCU read-side section.
