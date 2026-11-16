# PTE_SIZE RFC — execution plan

Goal: prepare an RFC patchset for LSF/MM that splits `PAGE_SIZE` into
`PG_SIZE` (what userspace sees) and `PTE_SIZE` (what one leaf PTE
maps), scoped to core-mm. The RFC is not self-contained; a git branch
containing treewide adaptations and an x86 enabler is linked so
reviewers can boot the full thing.

## Final tree layout (top to bottom reads like "what the branch is")

```
master
 ├── [arch cleanup: per-arch PTE_SHIFT collisions]
 ├── [mm: the RFC patchset]                                 ← posted to list
 ├── [treewide: cocci-driven PAGE_SIZE/SHIFT/MASK → PG_*]
 ├── [treewide-semantic: places we chose PTE over PG]       (small / optional)
 ├── [arch/x86: fixes needed to run PG_SIZE > PTE_SIZE]
 ├── [x86: allow selecting PG_SIZE]                         (flips the switch)
 └── [tools/testing/pte_size]
```

Only the `[mm: …]` block is posted as patches. Everything else lives
on the branch, linked from the cover letter.

## Scope

### In-scope for the RFC patchset

- `include/linux/{mm,mm_types,pfn,pagemap,pgtable,rmap,highmem}.h`
- `mm/memory.c`, `mm/filemap.c`, `mm/mmap.c`, `mm/vma.c`,
  `mm/mprotect.c`, `mm/mremap.c`, `mm/rmap.c`, `mm/interval_tree.c`

### On the branch, not posted

- **treewide (cocci)** — mechanical rename everywhere *except* what the
  RFC patchset touches. Covers fs/ (including fuse), drivers/, net/,
  non-x86 arches, etc.
- **treewide-semantic** — hand-written adjustments where
  `PAGE_SIZE` was really `PTE_SIZE` (or vice versa). Keep small.
- **arch/x86 fixes** — SMP realmode, `empty_zero_page` sizing,
  `flush_tlb_range`/`flush_tlb_page` stride, PTI double-PGD layout,
  `arch_get_unmapped_area` alignment, VDSO/vvar, ACPI tables, `brk`,
  munmap-across-PG, `text_poke`. All prerequisites for the demo to
  boot, none of them mm-design. Each gets its own thread later.
- **x86 enabler** — `CONFIG_X86_PG_SIZE_{4KB,16KB,64KB}`. Single
  commit. EXPERIMENTAL / depends on BROKEN if needed.
- **tools/testing/pte_size** — the initrd harness and C tests. Useful
  for "go run this" but not part of the mm argument.

## Cocci rule

The cocci rule renames `PAGE_SIZE`/`PAGE_SHIFT`/`PAGE_MASK`/
`PAGE_ALIGN`/`offset_in_page` to the `PG_*` / `offset_in_pg`
equivalents, and does *not* touch anything the RFC patchset modifies.

**Excluded paths** (listed in the cocci file's header comment so re-runs
are safe):

- `mm/**`
- `include/linux/mm.h`
- `include/linux/mm_types.h`
- `include/linux/pfn.h`
- `include/linux/pagemap.h`
- `include/linux/pgtable.h`
- `include/linux/rmap.h`
- `include/linux/highmem.h`

Rationale: mm has been hand-curated in the RFC series; running cocci
there would stomp decisions (e.g. places where PTE vs PG was a
deliberate choice). Cocci is applied to the rest of the tree and its
output is committed as the `[treewide]` commit, regenerated from
scratch after every rebase of the mm series.

## Fault-handler refactor (the "mess" to fix before posting)

Problems in the current state:

- **Two install paths for COW.** `do_cow_fault()` hands off to
  `finish_fault()`, which has a multi-PTE install loop with clamp
  logic. `wp_page_copy()` does a single-PTE install and then calls
  `cow_install_sibling_ptes()` afterwards. Same operation,
  implemented twice, with different clamp rules and different
  orderings wrt rmap/RSS.
- **`needs_fallback` goto maze.** `finish_fault()` has `fallback:`
  and `retry_single:` labels and a boolean that's set from three
  different sites. The last two mm bug-fix commits
  (`88101323`, `42714d28`) are one-liners patching invariants broken
  across the `fallback:` jump.
- **Dense clamp arithmetic.** Four conditions against `idx`,
  `vma_off`, `pte_off`. Correct, but opaque.

Refactor plan (each step is one preparatory patch, no behaviour change
for `PTE == PG`):

1. **Name the clamp inputs.** Replace `idx`, `start_idx`, `end_idx`,
   `vma_off`, `pte_off` with named struct fields (`pg_sub`,
   `vma_remaining_ptes`, `pt_remaining_ptes`) and a short comment
   stating the invariant: *we install `[start, end)` PTEs that all
   live in one PG of one folio, inside one VMA, inside one PT page.*

2. **Gate i_size fallback to file folios only.** Move the
   `folio_next_index` vs `file_end` check behind `!is_cow && vma->vm_file`.
   (This is what commit `42714d28` does as a fix; in the refactored
   series it becomes the natural shape of the code.)

3. **Extract `install_cow_ptes(vmf, new_folio, unshare)` helper.**
   Single entry point that computes the clamp, installs the primary
   PTE, installs siblings (tearing down stale file-backed PTEs,
   handling the "found anon → retry single" case), and updates rmap +
   RSS in one place. `cow_install_sibling_ptes()` disappears; both
   `wp_page_copy()` and `finish_fault()` call the new helper.

4. **Consolidate `cow_find_sibling_anon()`** to one call site (inside
   `install_cow_ptes()` or feeding it).

5. **Replace `needs_fallback` + gotos with an explicit helper.**
   `finish_fault()` becomes: compute clamp → call install helper →
   on "retry single" signal, fall through to the single-PTE path. No
   `fallback:`/`retry_single:` labels.

After these five patches, the `PG > PTE` delta on top is small: change
`1` → `PTES_PER_PAGE` in one place, plus the clamp math (which is now
readable).

## RFC patch order (the `[mm: …]` block)

Target: ~20 patches. Order below is the logical read-order; reviewers
bisect their reading, so logical bisectability matters even though
full-tree compile bisectability is not enforced (the branch has
treewide on top).

**Preparation — no behaviour change, types and accessors**

1. mm: introduce `PTE_SIZE`, `PTE_SHIFT`, `PTE_MASK`
2. mm: introduce `PG_SIZE`, `PG_SHIFT`, `PG_MASK`
3. mm: introduce `offset_in_pte()` and `offset_in_pg()`
4. mm: introduce `PTE_ALIGN()` and `PG_ALIGN()`
5. mm: introduce `mkpte()` and `folio_mkpte()`
6. mm: redefine PFN to be in `PTE_SIZE` units
7. mm: introduce `PTES_TO_PAGES()`, `PAGES_TO_PTES()`, `PTES_PER_PAGE`
8. mm: introduce `folio_nr_ptes()` and `vma_ptes()`
9. mm: introduce `linear_pte_index()` (alias `linear_page_index` when PTE==PG)
10. mm: introduce pteoff; add `vm_pteoff` (alias of `vm_pgoff` when PTE==PG)

**VMA plumbing — mechanical, still no behaviour change**

11. mm/vma: convert `vma_merge_struct` and `mmap_state` to pteoff
12. mm: update `vma_pages()` / `vma_ptes()`
13. mm/interval_tree: use `vma_ptes()` in `vma_last_pteoff()`

**Fault-handler refactor — Phase-B deliverables, still PTE==PG**

14. mm: name `finish_fault()` clamp inputs
15. mm: gate `finish_fault()` i_size fallback on file folios only
16. mm: extract `install_cow_ptes()` helper
17. mm: consolidate `cow_find_sibling_anon()` call sites
18. mm: replace `finish_fault()` fallback goto with predicate helper
19. mm: update `set_pte_range()` to take pteoff
20. mm: update `finish_fault()` to operate in pteoff units

**Enable PG != PTE — the interesting patches**

21. mm/rmap: count PTEs in folio mapcount sanity checks
22. mm/rmap: convert pgoff to pteoff for `__rmap_walk_file()`
23. mm: extend `is_zero_pfn()` across the whole PG
24. mm: map multiple PTEs per page in `do_anonymous_page()`
25. mm/filemap: fix batching in `filemap_map_folio_range()` for MAP_PRIVATE
26. mm: adapt page-fault handlers for `PG_SIZE > PTE_SIZE` (residual delta)

Patches 16–26 are the substance; 1–15 are preparation.

## Validation

Full-VM testing (not the minimalistic route):

- **4K build** (`build-4k/`): boots to login via `run-vm`; selftests
  15/15; `--run 'echo OK'` completes.
- **16K build** (`build-16k/`): boots to login; selftests 15/15.
  Known: `/lib/modules` mount fails unless `make modules_install
  INSTALL_MOD_PATH=build-16k` is done first — unrelated to mm.
- **64K build** (`build-64k/`): boots to login; selftests 15/15.
  Known: SMP bring-up is broken on this config; boot with `nosmp`.

Script: a one-liner wrapper that boots each build and greps dmesg for
`WARN|BUG:|Oops|panic|stall`. Run after every rebase or after the
treewide commit is regenerated.

## Cover letter skeleton

- Problem statement: what `PAGE_SIZE` conflates and why it hurts (larger
  userspace pages, mTHP accounting, large-folio work, …).
- Design: `PG_SIZE` user-visible, `PTE_SIZE` hw-visible; why this split.
- What's in this patchset vs on the branch (link to branch, list the
  extra commits at a summary level).
- Explicitly-deferred items list (fs rename via cocci, x86 bug fixes,
  x86 enabler, selftests, non-x86 arches).
- Testing matrix: `PTE == PG` on 4K/64K base (unchanged behaviour),
  `PG > PTE` on 16K-on-4K demo. Selftests referenced externally.
- Known limitations (SMP on 64K, other arches untouched).
- Concrete questions to discuss at LSF/MM.

## Work sequence for the next session

Execute in this order. Each step ends at a clean state (branch builds,
VMs boot, selftests pass).

### Step 1 — snapshot

- `git tag pte_size-pre-rfc` on the current `pte_size` branch head
  (`bf8675681c83`). This is the always-reachable "working" state.

### Step 2 — split the existing branch into the target layout

Starting from `pte_size-pre-rfc`, rebase commits onto the target
structure:

1. At the bottom of the branch: the per-arch `PTE_SHIFT` cleanups (the
   couple of commits that remove name collisions on powerpc/sparc/sh/
   parisc/x86). These are prerequisites for introducing the new macros
   on the whole tree; keep them separate from the RFC posting.
2. Above that: the RFC patch block (Phase C, 20-ish patches). Initially
   this is the *existing* series of `mm: Introduce …` / `mm: Update …`
   / `mm: Adapt …` commits from the branch; Step 4 will reorder and
   refactor them.
3. Above the RFC block: the treewide-mechanical commit (regenerated —
   see Step 3).
4. Optional treewide-semantic commit above that (empty at first; fill
   in if the treewide rerun leaves non-trivial diff that cocci can't
   handle).
5. The arch/x86 fix commits (SMP realmode, empty_zero_page, tlb,
   text_poke, VDSO/vvar, ACPI, brk, munmap, get_unmapped_area, …) as a
   run of small commits.
6. The x86 `allow selecting PG_SIZE` enabler — last arch commit.
7. `tools/testing/pte_size` tree as the final group.

The intent is to *reorder and relabel*, not rewrite, in this step.
Boot 4K/16K/64K at the end of Step 2 to confirm nothing regressed.

### Step 3 — write and apply the cocci rule

- Add `scripts/pte-size-rename.cocci` with excludes listed above.
- Regenerate the treewide commit from scratch by running cocci over a
  clean checkout at the tip of the RFC block. Commit with title
  `treewide: rename PAGE_SIZE/SHIFT/MASK to PG_* (cocci)` and include
  the cocci file in that commit.
- Walk the diff briefly to catch places cocci over-renamed (e.g.
  places where the code genuinely meant PTE). If any appear, carve a
  small hand-written `treewide-semantic` commit on top.

### Step 4 — execute the fault-handler refactor

Five preparatory patches in the Phase-B order above, inside the RFC
block. Each one small, each leaves the tree bootable. After all five,
the remaining "enable PG != PTE" patches (21–26) should shrink
considerably from their current form. Rewrite them against the
refactored code rather than preserving the current shape.

Boot and selftest after each of the five refactor patches on
`build-16k/` — that's the config where they bite hardest.

### Step 5 — finalise patch ordering, squash, and audit commit messages

- Reorder the RFC block into the final 20-patch sequence (Phase C
  order).
- Every commit message gets rewritten to explain *why*, not *what* —
  the diff shows what. Mention the invariant being preserved or
  introduced. Link to LSF/MM topic tag.
- Each commit title uses kernel conventions (`mm: foo: bar`, imperative
  mood, no trailing period).
- Drop or squash any "fix for a bug introduced two commits ago" —
  amend the introducing commit instead.

### Step 6 — draft and post

- Write the cover letter from the skeleton above.
- `git format-patch --cover-letter --rfc --subject-prefix='RFC PATCH'
  --to=linux-mm --cc=lsf-pc` etc.
- Before posting: one last pass of `scripts/checkpatch.pl` per patch,
  one clean build + boot cycle on 4K/16K/64K.

## Rules of thumb during execution

- **Don't over-narrow for bisect.** The tree has treewide + enabler on
  top, so every commit up to the top is buildable as a whole. Within
  the mm series, aim for *readable* per-commit, not *compilable*
  per-commit.
- **Regenerate treewide after every mm-series rebase.** Don't hand-edit
  the treewide commit.
- **When in doubt, leave it on the branch.** Anything that isn't a
  core-mm design argument belongs below the RFC block (for arch
  prereqs) or above (for treewide/enabler), not inside the RFC block.
- **Keep the test harness runnable.** `build-16k/` + `--run-timeout`
  boot + selftests is the canary. If it stops working during a
  rebase, stop and fix rather than pressing on.
