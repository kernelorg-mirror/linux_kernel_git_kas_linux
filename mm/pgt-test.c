// SPDX-License-Identifier: GPL-2.0
/*
 * Test helpers for the level-generic page table API, not meant for
 * merging: a build check of the whole interface and a walker benchmark.
 *
 * /sys/kernel/debug/pgt_bench times walks of a region it maps and
 * populates in the writer's mm. Writing "4k|thp SIZE_MB ROUNDS ITERS"
 * runs ROUNDS rounds of ITERS walks with each walker, in a different
 * order every round; reading prints the minimum and the median time per
 * walk. The walkers share one body:
 *
 *   pgt     PGT_DEFINE_LEVEL_WALKER()
 *   pgt2    an identical copy, for the null comparison
 *   rec     plain recursion, with the level known only at run time
 *   legacy  the per-level helpers
 */
#include <linux/debugfs.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/pgalloc.h>
#include <linux/pgt.h>
#include <linux/pgt-walk.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/uaccess.h>
#include <asm/tlb.h>

/*
 * A user of every function and macro of the consumer API, never called:
 * building it compiles the whole API, also the parts no consumer uses yet.
 */
struct pgt_exercise_ctx {
	struct mm_struct *mm;
	unsigned long n;
};

PGT_DEFINE_LEVEL_WALKER(pgt_exercise_walk, pgt_exercise_body,
			struct pgt_exercise_ctx *);

static __always_inline unsigned long
pgt_exercise_body(struct pgt_exercise_ctx *ctx, pgtp_t p, pgtp_t pp,
		  unsigned long addr, unsigned long end, const unsigned int lvl)
{
	unsigned long next;
	spinlock_t *ptl;
	pgtp_t c;
	pgtv_t v;

	do {
		next = pgtp_addr_end(p, addr, end);
		v = pgtp_get_lockless(p);
		if (lvl && pgtv_kind(v) == PGT_TABLE) {
			c = pgt_table_child(pgtv_to_table(v), addr);
			next = pgt_exercise_walk(ctx, c, p, addr, next, lvl - 1);
		} else if (!lvl) {
			ptl = pgtp_lock_table(ctx->mm, p, pp);
			if (!ptl)
				return addr;
			ctx->n += pgtv_kind(pgtp_get(p)) == PGT_LEAF;
			pgtp_unlock(p, ptl);
		}
	} while (p = pgtp_next(p), addr = next, addr != end);
	return end;
}

static void __maybe_unused pgt_exercise(struct mmu_gather *tlb,
					struct vm_area_struct *vma,
					struct folio *folio, softleaf_t e,
					unsigned long addr, unsigned long end)
{
	struct mm_struct *mm = vma->vm_mm;
	struct pgt_exercise_ctx ctx = { .mm = mm };
	pgtp_t p = pgtp_root(mm, addr), pp = PGTP_NULL;
	unsigned int lvl = pgtp_level(p);
	pgtv_t v = pgtp_get_lockless(p);
	pgt_softleaf_t s;
	pgt_leaf_t l;
	spinlock_t *ptl;
	PGT_ITER(it, mm, addr, end, PGT_ITER_LOCKED);

	pgt_exercise_walk(&ctx, p, pp, addr, end, lvl);

	if (pgtv_kind(v) == PGT_NONE && pgt_alloc_child(mm, p, &v))
		return;
	if (pgtv_kind(v) == PGT_BAD)
		pgtp_clear_bad(p);
	if (pgtv_kind(v) == PGT_TABLE) {
		pp = p;
		p = pgt_table_child(pgtv_to_table(v), addr);
	}
	p = pgtp_advance(pgtp_next(p), 0);
	ctx.n += pgt_root_level(mm) + pgtp_is_null(p) +
		 (unsigned long)pgtp_table(p);
	ctx.n += pgtp_level(pgtp_make(pgtp_ptr(p), pgtp_level(p)));
	pgtp_assume_level(p, pgtp_level(p));

	ptl = pgtp_lock_entry(mm, p);
	pgtp_assert_locked(mm, p);
	l = pgtv_to_leaf(pgtp_get(p));
	l = pgt_leaf_mk(pgt_leaf_pfn(l), pgt_leaf_pgprot(l), pgtp_level(p));
	ctx.n += pgt_leaf_shift(l) + pgt_leaf_size(l);
	l = pgt_leaf_modify(l, vma->vm_page_prot);
	l = pgt_leaf_mkwrite(pgt_leaf_mkwrite_novma(l), vma);
	l = pgt_leaf_mkclean(pgt_leaf_mkdirty(pgt_leaf_wrprotect(l)));
	l = pgt_leaf_mkold(pgt_leaf_mkyoung(pgt_leaf_mkspecial(l)));
	l = pgt_leaf_clear_soft_dirty(pgt_leaf_mksoft_dirty(l));
	l = pgt_leaf_clear_uffd_wp(pgt_leaf_mkuffd_wp(l));
	l = pgt_leaf_next(l);
	ctx.n += pgt_leaf_write(l) + pgt_leaf_dirty(l) +
		 pgt_leaf_young(l) + pgt_leaf_special(l) +
		 pgt_leaf_soft_dirty(l) + pgt_leaf_uffd_wp(l) +
		 pgt_leaf_exec(l) + pgt_leaf_user(l) + pgt_leaf_cont(l) +
		 pgt_leaf_accessible(l, true) +
		 pgtv_same(v, pgtv_from_leaf(l));
	pgtp_set_leaf(mm, addr, p, l);
	pgtp_set_leaf_nr(mm, addr, p, l, 1);
	s = pgt_softleaf_mk(e, pgtp_level(p));
	s = pgt_softleaf_clear_soft_dirty(pgt_softleaf_mksoft_dirty(s));
	s = pgt_softleaf_clear_uffd_wp(pgt_softleaf_mkuffd_wp(s));
	ctx.n += pgt_softleaf_soft_dirty(s) + pgt_softleaf_uffd_wp(s) +
		 softleaf_from_pgt(s).val;
	pgtp_set_softleaf(mm, addr, p, s);
	v = pgtp_xchg(mm, addr, p, pgtv_from_softleaf(s));
	s = pgtv_to_softleaf(v);
	v = pgtp_xchg(mm, addr, p, pgt_none_mk(pgtp_level(p)));
	l = pgtp_invalidate(vma, addr, p);
	pgtp_clear(mm, addr, p);
	tlb_remove_pgt_entry(tlb, p, addr);
	v = pgtv_from_table(pgt_table_mk(NULL, pgtp_level(p)));
	tlb_remove_pgt_table(tlb, pgtv_to_table(v), addr);
	ctx.n += pgt_leaf_batch(folio, p, l, 1);
	ctx.n += pgt_softleaf_batch(p, s, 1) + pgtp_size(p) + pgtv_level(v);
	l = pgtp_get_and_clear_leaves(vma, addr, p, 1, true);
	pgtp_clear_leaves(vma, addr, p, 1, false);
	pgtp_clear_softleaves(vma, addr, p, 1, false);
	pgt_leaf_check_zapped(vma, l);
	tlb_remove_pgt_entries(tlb, p, 1, addr);
	ctx.n += tlb_remove_pgt_pages(tlb, p, folio_page(folio, 0), 1, false);
	{
		size_t size;

		ctx.n += (unsigned long)pgtv_raw(&v, &size) + size;
	}
	pgtp_unlock(p, ptl);
	if (pgtp_map(p, pp))
		pgtp_unmap(p);

	for (v = pgt_iter_first(&it); !pgt_iter_done(&it); v = pgt_iter_next(&it)) {
		ctx.n += pgt_iter_addr(&it) + pgt_iter_size(&it) +
			 pgtp_level(pgt_iter_ptp(&it)) +
			 (pgtv_kind(v) == PGT_LEAF);
		if (ctx.n == e.val)
			pgt_iter_stop(&it);
	}
}

struct pgt_bench_ctx {
	struct mm_struct *mm;
	unsigned long n, young, dirty, pfns, soft;
};

/*
 * Read each entry, descend into tables, and sum a few bits of each leaf.
 * Entries are counted in a local, so that the counter does not get in the
 * way of the loop.
 */
#define PGT_BENCH_BODY(DOWN)						\
	unsigned long next, ret, cnt = 0;				\
	spinlock_t *ptl = NULL;						\
	pgtp_t first = p, child;					\
	pgt_table_t t;							\
	pgt_leaf_t l;							\
	pgtv_t v;							\
									\
	if (!lvl) {							\
		ptl = pgtp_lock_table(c->mm, p, pp);			\
		if (!ptl)						\
			return addr;					\
	}								\
	do {								\
		next = pgtp_addr_end(p, addr, end);			\
		v = lvl ? pgtp_get_lockless(p) : pgtp_get(p);		\
		cnt++;							\
		switch (pgtv_kind(v)) {					\
		case PGT_TABLE:						\
			if (!lvl)					\
				break;					\
			t = pgtv_to_table(v);				\
			child = pgt_table_child(t, addr);		\
			ret = DOWN(c, child, p, addr, next, lvl - 1);	\
			if (ret != next) {				\
				c->n += cnt;				\
				return ret;				\
			}						\
			break;						\
		case PGT_LEAF:						\
			l = pgtv_to_leaf(v);				\
			c->young += pgt_leaf_young(l);			\
			c->dirty += pgt_leaf_dirty(l);			\
			c->pfns += pgt_leaf_pfn(l);			\
			break;						\
		case PGT_SOFTLEAF:					\
			c->soft++;					\
			break;						\
		default:						\
			break;						\
		}							\
	} while (p = pgtp_next(p), addr = next, addr != end);		\
	if (ptl)							\
		pgtp_unlock(first, ptl);				\
	c->n += cnt;							\
	return end;

PGT_DEFINE_LEVEL_WALKER(pgt_bench_walk, pgt_bench_body,
			struct pgt_bench_ctx *);

static __always_inline unsigned long
pgt_bench_body(struct pgt_bench_ctx *c, pgtp_t p, pgtp_t pp,
	       unsigned long addr, unsigned long end, const unsigned int lvl)
{
	PGT_BENCH_BODY(pgt_bench_walk)
}

PGT_DEFINE_LEVEL_WALKER(pgt_bench_walk2, pgt_bench_body2,
			struct pgt_bench_ctx *);

static __always_inline unsigned long
pgt_bench_body2(struct pgt_bench_ctx *c, pgtp_t p, pgtp_t pp,
		unsigned long addr, unsigned long end, const unsigned int lvl)
{
	PGT_BENCH_BODY(pgt_bench_walk2)
}

static unsigned long pgt_bench_rec(struct pgt_bench_ctx *c, pgtp_t p,
				   pgtp_t pp, unsigned long addr,
				   unsigned long end, unsigned int lvl)
{
	PGT_BENCH_BODY(pgt_bench_rec)
}

static unsigned long pgt_bench_run_pgt(struct pgt_bench_ctx *c,
				       unsigned long addr, unsigned long end)
{
	pgtp_t root = pgtp_root(c->mm, addr);

	return pgt_bench_walk(c, root, PGTP_NULL, addr, end, pgtp_level(root));
}

static unsigned long pgt_bench_run_pgt2(struct pgt_bench_ctx *c,
					unsigned long addr, unsigned long end)
{
	pgtp_t root = pgtp_root(c->mm, addr);

	return pgt_bench_walk2(c, root, PGTP_NULL, addr, end, pgtp_level(root));
}

static unsigned long pgt_bench_run_rec(struct pgt_bench_ctx *c,
				       unsigned long addr, unsigned long end)
{
	pgtp_t root = pgtp_root(c->mm, addr);

	return pgt_bench_rec(c, root, PGTP_NULL, addr, end, pgtp_level(root));
}

/* The same walk with the per-level helpers */
static unsigned long pgt_bench_pte(struct pgt_bench_ctx *c, pmd_t *pmd,
				   unsigned long addr, unsigned long end)
{
	pte_t *start_pte, *pte, ptent;
	unsigned long cnt = 0;
	spinlock_t *ptl;

	start_pte = pte = pte_offset_map_lock(c->mm, pmd, addr, &ptl);
	if (!pte)
		return addr;
	do {
		cnt++;
		ptent = ptep_get(pte);
		if (pte_none(ptent))
			continue;
		if (!pte_present(ptent)) {
			c->soft++;
			continue;
		}
		c->young += pte_young(ptent);
		c->dirty += pte_dirty(ptent);
		c->pfns += pte_pfn(ptent);
	} while (pte++, addr += PAGE_SIZE, addr != end);
	pte_unmap_unlock(start_pte, ptl);
	c->n += cnt;
	return end;
}

static unsigned long pgt_bench_pmd(struct pgt_bench_ctx *c, pud_t *pud,
				   unsigned long addr, unsigned long end)
{
	pmd_t *pmd = pmd_offset(pud, addr);
	unsigned long next, ret, cnt = 0;
	pmd_t pmdval;

	do {
		next = pmd_addr_end(addr, end);
		cnt++;
		pmdval = pmdp_get_lockless(pmd);
		if (pmd_none(pmdval))
			continue;
		if (!pmd_present(pmdval)) {
			c->soft++;
			continue;
		}
		if (pmd_leaf(pmdval)) {
			c->young += pmd_young(pmdval);
			c->dirty += pmd_dirty(pmdval);
			c->pfns += pmd_pfn(pmdval);
			continue;
		}
		if (pmd_bad(pmdval))
			continue;
		ret = pgt_bench_pte(c, pmd, addr, next);
		if (ret != next) {
			c->n += cnt;
			return ret;
		}
	} while (pmd++, addr = next, addr != end);
	c->n += cnt;
	return end;
}

static unsigned long pgt_bench_pud(struct pgt_bench_ctx *c, p4d_t *p4d,
				   unsigned long addr, unsigned long end)
{
	pud_t *pud = pud_offset(p4d, addr);
	unsigned long next, ret;
	pud_t pudval;

	do {
		next = pud_addr_end(addr, end);
		pudval = pudp_get(pud);
		/* Not every architecture has PUD leaf helpers; none are mapped */
		if (pud_none(pudval) || pud_leaf(pudval) || pud_bad(pudval))
			continue;
		ret = pgt_bench_pmd(c, pud, addr, next);
		if (ret != next)
			return ret;
	} while (pud++, addr = next, addr != end);
	return end;
}

static unsigned long pgt_bench_p4d(struct pgt_bench_ctx *c, pgd_t *pgd,
				   unsigned long addr, unsigned long end)
{
	p4d_t *p4d = p4d_offset(pgd, addr);
	unsigned long next, ret;

	do {
		next = p4d_addr_end(addr, end);
		if (p4d_none_or_clear_bad(p4d))
			continue;
		ret = pgt_bench_pud(c, p4d, addr, next);
		if (ret != next)
			return ret;
	} while (p4d++, addr = next, addr != end);
	return end;
}

static unsigned long pgt_bench_run_legacy(struct pgt_bench_ctx *c,
					  unsigned long addr,
					  unsigned long end)
{
	pgd_t *pgd = pgd_offset(c->mm, addr);
	unsigned long next, ret;

	do {
		next = pgd_addr_end(addr, end);
		if (pgd_none_or_clear_bad(pgd))
			continue;
		ret = pgt_bench_p4d(c, pgd, addr, next);
		if (ret != next)
			return ret;
	} while (pgd++, addr = next, addr != end);
	return end;
}

static const struct {
	const char *name;
	unsigned long (*run)(struct pgt_bench_ctx *c, unsigned long addr,
			     unsigned long end);
} pgt_bench_walkers[] = {
	{ "pgt", pgt_bench_run_pgt },
	{ "pgt2", pgt_bench_run_pgt2 },
	{ "rec", pgt_bench_run_rec },
	{ "legacy", pgt_bench_run_legacy },
};

#define PGT_BENCH_NR		ARRAY_SIZE(pgt_bench_walkers)
#define PGT_BENCH_MAX_ROUNDS	1000

static char pgt_bench_out[1024];

/* Time @iters walks of [@start, @end) with walker @k; ns per walk */
static u64 pgt_bench_time(unsigned int k, unsigned long start,
			  unsigned long end, unsigned long iters,
			  unsigned long *entries)
{
	struct pgt_bench_ctx c = { .mm = current->mm };
	unsigned long i, addr;
	u64 t0, t1;

	mmap_read_lock(c.mm);
	t0 = ktime_get_ns();
	for (i = 0; i < iters; i++) {
		for (addr = start; addr < end; )
			addr = pgt_bench_walkers[k].run(&c, addr, end);
	}
	t1 = ktime_get_ns();
	mmap_read_unlock(c.mm);
	*entries = c.n / iters;
	return div64_u64(t1 - t0, iters);
}

static int pgt_bench_cmp(const void *a, const void *b)
{
	u64 x = *(const u64 *)a, y = *(const u64 *)b;

	return x < y ? -1 : x > y;
}

static void pgt_bench_report(const char *mode, unsigned long size,
			     unsigned long rounds, unsigned long iters,
			     u64 *times, unsigned long entries)
{
	unsigned int k, len;
	u64 *t, med;

	len = scnprintf(pgt_bench_out, sizeof(pgt_bench_out),
			"%s %luMB rounds %lu iters %lu entries/walk %lu\n"
			"walker  min_ns/walk  median_ns/walk  median_ps/entry\n",
			mode, size >> 20, rounds, iters, entries);
	for (k = 0; k < PGT_BENCH_NR; k++) {
		t = times + k * rounds;
		sort(t, rounds, sizeof(*t), pgt_bench_cmp, NULL);
		med = t[rounds / 2];
		len += scnprintf(pgt_bench_out + len,
				 sizeof(pgt_bench_out) - len,
				 "%-6s  %11llu  %14llu  %15llu\n",
				 pgt_bench_walkers[k].name, t[0], med,
				 div64_u64(med * 1000, max(entries, 1UL)));
	}
}

static ssize_t pgt_bench_write(struct file *file, const char __user *ubuf,
			       size_t len, loff_t *ppos)
{
	unsigned long size, rounds, iters, map, start, end, entries, n;
	unsigned int r, i, k;
	char buf[64], mode[8];
	u64 *times;
	bool thp;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = 0;
	if (sscanf(buf, "%7s %lu %lu %lu", mode, &size, &rounds, &iters) != 4)
		return -EINVAL;
	thp = !strcmp(mode, "thp");
	if ((!thp && strcmp(mode, "4k")) || !size || !iters || !rounds ||
	    rounds > PGT_BENCH_MAX_ROUNDS)
		return -EINVAL;
	size <<= 20;

	times = kcalloc(PGT_BENCH_NR * rounds, sizeof(*times), GFP_KERNEL);
	if (!times)
		return -ENOMEM;
	map = vm_mmap(NULL, 0, size + PMD_SIZE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, 0);
	if (IS_ERR_VALUE(map)) {
		kfree(times);
		return map;
	}
	start = ALIGN(map, PMD_SIZE);
	end = start + size;
	do_madvise(current->mm, start, size, thp ? MADV_HUGEPAGE :
						     MADV_NOHUGEPAGE);
	mm_populate(start, size);

	/* One untimed round, then each round in a different order */
	for (k = 0; k < PGT_BENCH_NR; k++)
		pgt_bench_time(k, start, end, iters, &n);
	entries = 0;
	for (r = 0; r < rounds; r++) {
		for (i = 0; i < PGT_BENCH_NR; i++) {
			k = (i + r) % PGT_BENCH_NR;
			times[k * rounds + r] = pgt_bench_time(k, start, end,
							       iters, &n);
			if (!k)
				entries = n;
			cond_resched();
		}
	}
	pgt_bench_report(mode, size, rounds, iters, times, entries);
	vm_munmap(map, size + PMD_SIZE);
	kfree(times);
	return len;
}

static ssize_t pgt_bench_read(struct file *file, char __user *ubuf,
			      size_t len, loff_t *ppos)
{
	return simple_read_from_buffer(ubuf, len, ppos, pgt_bench_out,
				       strlen(pgt_bench_out));
}

static const struct file_operations pgt_bench_fops = {
	.write = pgt_bench_write,
	.read = pgt_bench_read,
	.llseek = default_llseek,
};

static int __init pgt_bench_init(void)
{
	debugfs_create_file("pgt_bench", 0600, NULL, NULL, &pgt_bench_fops);
	return 0;
}
late_initcall(pgt_bench_init);
