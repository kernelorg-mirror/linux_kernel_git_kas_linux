// SPDX-License-Identifier: GPL-2.0
/*
 * Level-generic page table API: the out-of-line parts of the consumer API.
 *
 * Everything here is built on the pgt_arch_*() contract and never looks
 * inside an entry. See Documentation/mm/pgt.rst.
 */
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/pgt.h>
#include <linux/pgt-walk.h>
#include <linux/rcupdate.h>

/*
 * With split 64-bit entries read in two halves, a lockless read of a level-1
 * entry is only known to come from matched halves while interrupts are off:
 * that holds off the TLB flush between two present values.
 */
#if defined(CONFIG_GUP_GET_PXX_LOW_HIGH) && \
	(defined(CONFIG_SMP) || defined(CONFIG_PREEMPT_RCU))
#define pgt_irq_save(flags)	local_irq_save(flags)
#define pgt_irq_restore(flags)	local_irq_restore(flags)
#else
#define pgt_irq_save(flags)	((void)(flags))
#define pgt_irq_restore(flags)	((void)(flags))
#endif

struct ptdesc *pgtp_table(pgtp_t p)
{
	void *ptr = pgtp_ptr(p);

#ifdef CONFIG_HIGHPTE
	if (!pgtp_level(p)) {
		struct page *page = kmap_to_page(ptr);

		return page_ptdesc(page);
	}
#endif
	return virt_to_ptdesc(ptr);
}

static bool pgt_parent_references(pgtp_t pp, pgtp_t p)
{
	unsigned long flags = 0;
	pgtv_t v;

	pgt_irq_save(flags);
	v = pgtp_get_lockless(pp);
	pgt_irq_restore(flags);

	return pgtv_kind(v) == PGT_TABLE && pgt_arch_links(v, p);
}

static void pgt_unmap_table(pgtp_t p)
{
#ifdef CONFIG_HIGHPTE
	if (!pgtp_level(p))
		kunmap_local(pgtp_ptr(p));
#endif
}

/*
 * The first check runs before pgtp_lockptr() reads the ptdesc of the table:
 * the table may have been freed between the caller's snapshot of *pp and
 * rcu_read_lock(). Once it is seen linked inside the RCU section it cannot
 * be freed before rcu_read_unlock(). The second check, under the lock, is
 * the one that makes the locked table the live one.
 */
spinlock_t *pgtp_lock_table(struct mm_struct *mm, pgtp_t p, pgtp_t pp)
{
	spinlock_t *ptl;

	VM_WARN_ON_ONCE(pgtp_level(p));
	rcu_read_lock();
	if (!pgtp_is_null(pp) && !pgt_parent_references(pp, p))
		goto fail;
	ptl = pgtp_lockptr(mm, p);
	spin_lock(ptl);
	if (pgtp_is_null(pp) || pgt_parent_references(pp, p))
		return ptl;
	spin_unlock(ptl);
fail:
	pgt_unmap_table(p);
	rcu_read_unlock();
	return NULL;
}

spinlock_t *pgtp_lock_entry(struct mm_struct *mm, pgtp_t p)
{
	spinlock_t *ptl;

	VM_WARN_ON_ONCE(!pgtp_level(p));
	ptl = pgtp_lockptr(mm, p);
	spin_lock(ptl);
	return ptl;
}

void pgtp_unlock(pgtp_t p, spinlock_t *ptl)
{
	spin_unlock(ptl);
	if (pgtp_level(p))
		return;
	pgt_unmap_table(p);
	rcu_read_unlock();
}

bool pgtp_map(pgtp_t p, pgtp_t pp)
{
	rcu_read_lock();
	if (pgtp_is_null(pp) || pgt_parent_references(pp, p))
		return true;
	pgt_unmap_table(p);
	rcu_read_unlock();
	return false;
}

void pgtp_unmap(pgtp_t p)
{
	pgt_unmap_table(p);
	rcu_read_unlock();
}

int pgt_alloc_child(struct mm_struct *mm, pgtp_t p, pgtv_t *v)
{
	int ret;

	VM_WARN_ON_ONCE(!pgtp_level(p));
	ret = pgt_arch_alloc_child(mm, p);
	*v = pgtp_get_lockless(p);
	return ret;
}

void pgtp_clear_bad(pgtp_t p)
{
	pgt_arch_clear_bad(p);
}

unsigned int pgt_leaf_batch(struct folio *folio, pgtp_t p, pgt_leaf_t l,
			    unsigned int max_nr)
{
	VM_WARN_ON_ONCE(pgtp_level(p) || l.v.lvl);
	return pgt_arch_leaf_batch(folio, p, l.v, max_nr);
}

/* Iterator */

/*
 * The root level comes from the root pointer: on s390 an mm can gain a
 * level, and the two must not come from different moments.
 */
static void pgt_iter_restart(struct pgt_iter *it)
{
	pgtp_t root = pgtp_root(it->mm, it->addr);

	it->root = it->lvl = pgtp_level(root);
	it->path[it->lvl] = root;
}

/* Release the level-0 table the iterator holds */
static void pgt_iter_release(struct pgt_iter *it)
{
	if (it->ptl)
		pgtp_unlock(it->path[0], it->ptl);
	else
		pgtp_unmap(it->path[0]);
	it->ptl = NULL;
}

static void pgt_iter_finish(struct pgt_iter *it)
{
	it->addr = it->end;
	if (it->flags & PGT_ITER_RO)
		rcu_read_unlock();
}

/*
 * Descend from it->lvl through TABLE entries to the first entry of
 * another kind. A level-0 table is entered with pgtp_lock_table() or
 * pgtp_map(); when that fails the walk starts again from the root.
 */
static pgtv_t pgt_iter_descend(struct pgt_iter *it)
{
	for (;;) {
		pgtp_t p = it->path[it->lvl];
		pgtv_t v = it->ptl ? pgtp_get(p) : pgtp_get_lockless(p);
		pgt_table_t t;
		pgtp_t c;

		if (!it->lvl || pgtv_kind(v) != PGT_TABLE) {
			it->cur = v;
			return v;
		}
		t = pgtv_to_table(v);
		c = pgt_table_child(t, it->addr);
		if (it->lvl == 1) {
			if (it->flags & PGT_ITER_LOCKED)
				it->ptl = pgtp_lock_table(it->mm, c, p);
			if (it->flags & PGT_ITER_LOCKED ? !it->ptl :
			    !pgtp_map(c, p)) {
				pgt_iter_restart(it);
				continue;
			}
		}
		it->lvl--;
		it->path[it->lvl] = c;
	}
}

pgtv_t pgt_iter_first(struct pgt_iter *it)
{
	it->ptl = NULL;
	if (it->flags & PGT_ITER_RO)
		rcu_read_lock();
	if (it->addr >= it->end) {
		pgt_iter_finish(it);
		return pgt_none_mk(0);
	}
	pgt_iter_restart(it);
	return pgt_iter_descend(it);
}

pgtv_t pgt_iter_next(struct pgt_iter *it)
{
	unsigned long next;

	next = pgtp_addr_end(it->path[it->lvl], it->addr, it->end);
	while (it->lvl < it->root &&
	       next == pgtp_addr_end(it->path[it->lvl + 1], it->addr, it->end)) {
		if (!it->lvl)
			pgt_iter_release(it);
		it->lvl++;
	}
	if (next == it->end) {
		if (!it->lvl)
			pgt_iter_release(it);
		pgt_iter_finish(it);
		return pgt_none_mk(it->lvl);
	}
	it->addr = next;
	it->path[it->lvl] = pgtp_next(it->path[it->lvl]);
	return pgt_iter_descend(it);
}

bool pgt_iter_done(struct pgt_iter *it)
{
	return it->addr == it->end;
}

void pgt_iter_stop(struct pgt_iter *it)
{
	if (pgt_iter_done(it))
		return;
	if (!it->lvl)
		pgt_iter_release(it);
	pgt_iter_finish(it);
}

unsigned long pgt_iter_addr(struct pgt_iter *it)
{
	return it->addr;
}

unsigned long pgt_iter_size(struct pgt_iter *it)
{
	return pgtp_addr_end(it->path[it->lvl], it->addr, it->end) - it->addr;
}

pgtp_t pgt_iter_ptp(struct pgt_iter *it)
{
	return it->path[it->lvl];
}

unsigned int pgt_softleaf_batch(pgtp_t p, pgt_softleaf_t s,
				unsigned int max_nr)
{
	pgtv_t expected = pgt_arch_softleaf_next(s.v);
	unsigned int nr;

	for (nr = 1; nr < max_nr; nr++) {
		p = pgtp_next(p);
		if (!pgtv_same(pgtp_get(p), expected))
			break;
		expected = pgt_arch_softleaf_next(expected);
	}
	return nr;
}
