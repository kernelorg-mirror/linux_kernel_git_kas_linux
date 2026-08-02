// SPDX-License-Identifier: GPL-2.0
/*
 * mem-usemem.c
 *
 * usemem: a parameterized memory workload.
 *
 * Threads share one address space and each run a weighted mix of operations
 * over a long-lived region, timing every one of them.  What comes out is a
 * latency distribution per kind of operation, because that is where a change to
 * the memory management code shows itself: an average is dominated by the
 * operations that hit no slow path at all, while a workload notices the few
 * that wait for a lock, a fault or a page of memory.
 */

#include "bench.h"

#include <subcmd/parse-options.h>
#include "util/mutex.h"
#include "util/string2.h"
#include <api/fs/fs.h>

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/time64.h>

/*
 * The madvise() behaviours a workload asks for by hand, defined here for a
 * libc that predates them.
 */
#ifndef MADV_FREE
# define MADV_FREE	8
#endif
#ifndef MADV_COLD
# define MADV_COLD	20
#endif
#ifndef MADV_PAGEOUT
# define MADV_PAGEOUT	21
#endif
#ifndef MADV_COLLAPSE
# define MADV_COLLAPSE	25
#endif

/*
 * An operation that found nothing in the region to work on is not a failure and
 * is not timed: it is the mix asking for more than the region can offer, an mmap
 * with every chunk already mapped or an munmap with the share of them it may
 * unmap used up.  A failure is the kernel refusing, and worth telling apart.
 */
enum usemem_result {
	USEMEM_DONE,
	USEMEM_NOTHING,
	USEMEM_FAILED,
};

enum usemem_op_id {
	OP_READ,
	OP_WRITE,
	OP_MMAP,
	OP_MUNMAP,
	OP_MPROTECT,
	OP_DONTNEED,
	OP_FREE,
	OP_COLD,
	OP_PAGEOUT,
	OP_HUGEPAGE,
	OP_NOHUGEPAGE,
	OP_COLLAPSE,
	NR_OPS
};

/*
 * Log-linear buckets: HIST_SUB_BUCKETS of them per power of two, which keeps a
 * reported percentile within a sixteenth of its true value.  Forty powers of
 * two of nanoseconds reach eighteen minutes, well past anything one operation
 * can take.
 */
#define HIST_SUB_BITS		4
#define HIST_SUB_BUCKETS	(1U << HIST_SUB_BITS)
#define HIST_EXPONENTS		40
#define HIST_BUCKETS		((HIST_EXPONENTS - HIST_SUB_BITS + 1) * \
				 HIST_SUB_BUCKETS)

struct hist {
	u64	bucket[HIST_BUCKETS];
	u64	nr;
	u64	sum;
	u64	max;
};

struct usemem_thread {
	pthread_t	thread;
	unsigned int	nr;
	u8		*region;
	unsigned long	nr_pages;
	unsigned long	cursor;
	u64		rnd_state;
	u64		nr_iters;
	struct hist	hist[NR_OPS];
	u64		nothing[NR_OPS];
	u64		failed[NR_OPS];
	int		error[NR_OPS];
	int		last_error;
	u64		sink;

	/* Which chunks of the region are mapped, and how many are not */
	bool		*mapped;
	unsigned int	nr_holes;
};

static const char	*size_str	= "128MB";
static const char	*chunk_str	= "2MB";
static const char	*ops_str	= "read,write";
static bool		access_seq;
static unsigned int	holes_pct	= 25;
static unsigned int	nr_threads	= 1;
static unsigned int	nr_secs		= 5;
static unsigned long	nr_loops;
static unsigned int	seed		= 1;
static int		thp;
static bool		populate	= true;

static const struct option options[] = {
	OPT_STRING('s', "size", &size_str, "128MB",
		   "Size of the region each thread works on. "
		   "Available units: B, KB, MB, GB and TB (case insensitive)"),
	OPT_STRING('k', "chunk", &chunk_str, "2MB",
		   "Size of the chunk the address space operations work on. "
		   "Available units: B, KB, MB, GB and TB (case insensitive)"),
	OPT_STRING('o', "ops", &ops_str, "read,write",
		   "Operation mix, as a comma separated list of "
		   "<operation>[:<weight>]; \"help\" lists the operations"),
	OPT_UINTEGER(0, "holes", &holes_pct,
		     "Share of a region munmap may leave unmapped, "
		     "as a percentage (default: 25)"),
	OPT_UINTEGER('t', "threads", &nr_threads,
		     "Number of threads to run (default: 1)"),
	OPT_UINTEGER('r', "runtime", &nr_secs,
		     "Seconds to run for, 0 for no limit (default: 5)"),
	OPT_ULONG('l', "loops", &nr_loops,
		  "Operations to run per thread, 0 for no limit (default: 0)"),
	OPT_BOOLEAN(0, "sequential", &access_seq,
		    "Walk the region in order rather than at random"),
	OPT_UINTEGER('S', "seed", &seed,
		     "Seed the per-thread random number generators (default: 1)"),
	OPT_INTEGER('H', "thp", &thp,
		    "Huge page hint for the regions: "
		    "MADV_NOHUGEPAGE < 0 < MADV_HUGEPAGE"),
	OPT_BOOLEAN(0, "populate", &populate,
		    "Fault the regions in before measuring (default: yes)"),
	OPT_END()
};

static const char * const bench_mem_usemem_usage[] = {
	"perf bench mem usemem <options>",
	NULL
};

static unsigned int	op_weight[NR_OPS];
static unsigned int	total_weight;
static size_t		region_size;
static size_t		page_size;
static size_t		align_size;
static size_t		chunk_size;
static unsigned int	nr_chunks;
static unsigned int	max_holes;

static struct mutex	start_lock;
static struct cond	start_parent, start_worker;
static unsigned int	threads_starting;
static bool		done;

static u64 now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

/* xorshift64*, so that a run repeats given the same seed */
static u64 rnd(struct usemem_thread *t)
{
	u64 x = t->rnd_state;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	t->rnd_state = x;

	return x * 2685821657736338717ULL;
}

static unsigned int hist_bucket(u64 ns)
{
	unsigned int exp, idx;

	if (ns < HIST_SUB_BUCKETS)
		return ns;

	exp = 63 - __builtin_clzll(ns);
	idx = ((exp - HIST_SUB_BITS + 1) << HIST_SUB_BITS) +
		((ns >> (exp - HIST_SUB_BITS)) & (HIST_SUB_BUCKETS - 1));

	return idx < HIST_BUCKETS ? idx : HIST_BUCKETS - 1;
}

/* The smallest value the bucket holds; one less than it is in the one below */
static u64 hist_bucket_floor(unsigned int idx)
{
	unsigned int exp, sub;

	if (idx < HIST_SUB_BUCKETS)
		return idx;

	exp = (idx >> HIST_SUB_BITS) + HIST_SUB_BITS - 1;
	sub = idx & (HIST_SUB_BUCKETS - 1);

	return (u64)(HIST_SUB_BUCKETS + sub) << (exp - HIST_SUB_BITS);
}

static void hist_add(struct hist *h, u64 ns)
{
	h->bucket[hist_bucket(ns)]++;
	h->nr++;
	h->sum += ns;
	if (ns > h->max)
		h->max = ns;
}

static void hist_merge(struct hist *to, const struct hist *from)
{
	unsigned int i;

	for (i = 0; i < HIST_BUCKETS; i++)
		to->bucket[i] += from->bucket[i];
	to->nr += from->nr;
	to->sum += from->sum;
	if (from->max > to->max)
		to->max = from->max;
}

/*
 * Report a percentile as the largest value its bucket can hold, so it is an
 * upper bound on that share of the operations rather than a value that may sit
 * below some of them.
 */
static u64 hist_percentile(const struct hist *h, double pct)
{
	u64 target, seen = 0;
	unsigned int i;

	if (!h->nr)
		return 0;

	target = h->nr * pct / 100.0;
	if (!target)
		target = 1;

	for (i = 0; i < HIST_BUCKETS; i++) {
		seen += h->bucket[i];
		if (seen >= target) {
			u64 ceiling = hist_bucket_floor(i + 1) - 1;

			return ceiling < h->max ? ceiling : h->max;
		}
	}

	return h->max;
}

/*
 * Scan for a chunk in the state an operation needs, starting where it asked, so
 * that an operation gives up only when the whole region holds nothing for it.
 */
static int chunk_from(struct usemem_thread *t, unsigned int start, bool mapped)
{
	unsigned int i;

	for (i = 0; i < nr_chunks; i++) {
		unsigned int chunk = (start + i) % nr_chunks;

		if (t->mapped[chunk] == mapped)
			return chunk;
	}

	return -1;
}

static int pick_chunk(struct usemem_thread *t, bool mapped)
{
	return chunk_from(t, rnd(t) % nr_chunks, mapped);
}

/* One page per access, so a fault dominates the operation when there is one */
static u8 *pick_page(struct usemem_thread *t)
{
	unsigned long pages_per_chunk = chunk_size / page_size;
	unsigned long page;
	int chunk;

	if (access_seq)
		page = t->cursor++ % t->nr_pages;
	else
		page = rnd(t) % t->nr_pages;

	chunk = page / pages_per_chunk;
	if (!t->mapped[chunk]) {
		/*
		 * The page fell in a hole.  Keep its offset within the chunk but
		 * move to one that is mapped, of which a region always has one.
		 */
		chunk = chunk_from(t, chunk, true);
		page = (unsigned long)chunk * pages_per_chunk +
			page % pages_per_chunk;
	}

	return t->region + page * page_size;
}

static enum usemem_result usemem_failed(struct usemem_thread *t)
{
	t->last_error = errno;

	return USEMEM_FAILED;
}

/* One word per cache line: the whole page is touched, none of it twice */
static enum usemem_result op_read(struct usemem_thread *t)
{
	const u64 *p = (const u64 *)pick_page(t);
	u64 sum = 0;
	size_t i;

	for (i = 0; i < page_size / sizeof(*p); i += 8)
		sum += p[i];

	t->sink += sum;

	return USEMEM_DONE;
}

static enum usemem_result op_write(struct usemem_thread *t)
{
	u64 *p = (u64 *)pick_page(t);
	size_t i;

	for (i = 0; i < page_size / sizeof(*p); i += 8)
		p[i] = t->nr_iters;

	return USEMEM_DONE;
}

static enum usemem_result op_mmap(struct usemem_thread *t)
{
	int chunk = pick_chunk(t, false);
	u8 *addr;

	if (chunk < 0)
		return USEMEM_NOTHING;

	addr = t->region + chunk * chunk_size;
	if (mmap(addr, chunk_size, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED)
		return usemem_failed(t);

	/* The new mapping does not inherit the hint the region was given */
	if (thp)
		madvise(addr, chunk_size,
			thp > 0 ? MADV_HUGEPAGE : MADV_NOHUGEPAGE);

	t->mapped[chunk] = true;
	t->nr_holes--;

	return USEMEM_DONE;
}

static enum usemem_result op_munmap(struct usemem_thread *t)
{
	int chunk;

	if (t->nr_holes >= max_holes)
		return USEMEM_NOTHING;

	chunk = pick_chunk(t, true);
	if (chunk < 0)
		return USEMEM_NOTHING;

	if (munmap(t->region + chunk * chunk_size, chunk_size))
		return usemem_failed(t);

	t->mapped[chunk] = false;
	t->nr_holes++;

	return USEMEM_DONE;
}

/* Read-only and straight back: what a workload watching for writes does */
static enum usemem_result op_mprotect(struct usemem_thread *t)
{
	int chunk = pick_chunk(t, true);
	u8 *addr;

	if (chunk < 0)
		return USEMEM_NOTHING;

	addr = t->region + chunk * chunk_size;
	if (mprotect(addr, chunk_size, PROT_READ))
		return usemem_failed(t);

	if (mprotect(addr, chunk_size, PROT_READ | PROT_WRITE)) {
		/* The chunk would stay read-only and take the next write with it */
		fprintf(stderr, "Cannot restore write access: %s\n",
			strerror(errno));
		done = true;
		return usemem_failed(t);
	}

	return USEMEM_DONE;
}

static enum usemem_result op_advise(struct usemem_thread *t, int advice)
{
	int chunk = pick_chunk(t, true);

	if (chunk < 0)
		return USEMEM_NOTHING;

	if (madvise(t->region + chunk * chunk_size, chunk_size, advice))
		return usemem_failed(t);

	return USEMEM_DONE;
}

static enum usemem_result op_dontneed(struct usemem_thread *t)
{
	return op_advise(t, MADV_DONTNEED);
}

static enum usemem_result op_free(struct usemem_thread *t)
{
	return op_advise(t, MADV_FREE);
}

static enum usemem_result op_cold(struct usemem_thread *t)
{
	return op_advise(t, MADV_COLD);
}

static enum usemem_result op_pageout(struct usemem_thread *t)
{
	return op_advise(t, MADV_PAGEOUT);
}

static enum usemem_result op_hugepage(struct usemem_thread *t)
{
	return op_advise(t, MADV_HUGEPAGE);
}

static enum usemem_result op_nohugepage(struct usemem_thread *t)
{
	return op_advise(t, MADV_NOHUGEPAGE);
}

static enum usemem_result op_collapse(struct usemem_thread *t)
{
	return op_advise(t, MADV_COLLAPSE);
}

static const struct usemem_op {
	const char *name;
	const char *desc;
	enum usemem_result (*run)(struct usemem_thread *t);
	bool chunked;
} usemem_ops[NR_OPS] = {
	[OP_READ]	= { "read",  "Read one page of the region",  op_read  },
	[OP_WRITE]	= { "write", "Write one page of the region", op_write },
	[OP_MMAP]	= { "mmap", "Map a chunk that is unmapped",
			    op_mmap, true },
	[OP_MUNMAP]	= { "munmap", "Unmap a chunk", op_munmap, true },
	[OP_MPROTECT]	= { "mprotect", "Turn a chunk read-only and back",
			    op_mprotect, true },
	[OP_DONTNEED]	= { "dontneed", "madvise(MADV_DONTNEED) a chunk",
			    op_dontneed, true },
	[OP_FREE]	= { "free", "madvise(MADV_FREE) a chunk",
			    op_free, true },
	[OP_COLD]	= { "cold", "madvise(MADV_COLD) a chunk",
			    op_cold, true },
	[OP_PAGEOUT]	= { "pageout", "madvise(MADV_PAGEOUT) a chunk",
			    op_pageout, true },
	[OP_HUGEPAGE]	= { "hugepage", "madvise(MADV_HUGEPAGE) a chunk",
			    op_hugepage, true },
	[OP_NOHUGEPAGE]	= { "nohugepage", "madvise(MADV_NOHUGEPAGE) a chunk",
			    op_nohugepage, true },
	[OP_COLLAPSE]	= { "collapse", "madvise(MADV_COLLAPSE) a chunk",
			    op_collapse, true },
};

static enum usemem_op_id pick_op(struct usemem_thread *t)
{
	unsigned int draw = rnd(t) % total_weight;
	enum usemem_op_id op;

	for (op = 0; op < NR_OPS - 1; op++) {
		if (draw < op_weight[op])
			break;
		draw -= op_weight[op];
	}

	return op;
}

static void *worker_thread(void *arg)
{
	struct usemem_thread *t = arg;

	mutex_lock(&start_lock);
	if (!--threads_starting)
		cond_signal(&start_parent);
	cond_wait(&start_worker, &start_lock);
	mutex_unlock(&start_lock);

	while (!done) {
		enum usemem_op_id op = pick_op(t);
		enum usemem_result res;
		u64 start, end;

		start = now_ns();
		res = usemem_ops[op].run(t);
		end = now_ns();

		if (res == USEMEM_DONE) {
			hist_add(&t->hist[op], end - start);
		} else if (res == USEMEM_NOTHING) {
			t->nothing[op]++;
		} else {
			t->failed[op]++;
			if (!t->error[op])
				t->error[op] = t->last_error;
		}

		t->nr_iters++;
		if (nr_loops && t->nr_iters >= nr_loops)
			break;
	}

	return NULL;
}

/*
 * Align the region to a huge page, or a workload told to ask for huge pages
 * would never be given one, and read the size rather than assuming it: it is
 * 2MB on x86 but 512MB on arm64 with 64K pages.
 */
static size_t huge_page_size(void)
{
	unsigned long long size;

	if (sysfs__read_ull("kernel/mm/transparent_hugepage/hpage_pmd_size",
			    &size) < 0)
		return 2 * 1024 * 1024;

	return size;
}

static u8 *alloc_region(void)
{
	unsigned long addr;
	u8 *map;

	map = mmap(NULL, region_size + align_size, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (map == MAP_FAILED)
		return NULL;

	addr = ((unsigned long)map + align_size - 1) & ~(align_size - 1);
	if (addr != (unsigned long)map)
		munmap(map, addr - (unsigned long)map);
	munmap((void *)(addr + region_size),
	       (unsigned long)map + align_size - addr);
	map = (u8 *)addr;

	if (thp && madvise(map, region_size,
			   thp > 0 ? MADV_HUGEPAGE : MADV_NOHUGEPAGE) < 0) {
		fprintf(stderr, "Huge page hint refused: %s\n", strerror(errno));
		munmap(map, region_size);
		return NULL;
	}

	if (populate)
		memset(map, 0, region_size);

	return map;
}

static int parse_ops(void)
{
	char *copy, *save = NULL, *tok;
	int ret = 0;

	if (!strcmp(ops_str, "help")) {
		unsigned int i;

		printf("Available operations:\n\n");
		for (i = 0; i < NR_OPS; i++)
			printf("  %-12s %s\n", usemem_ops[i].name,
			       usemem_ops[i].desc);
		return -EAGAIN;
	}

	copy = strdup(ops_str);
	if (!copy)
		return -ENOMEM;

	for (tok = strtok_r(copy, ",", &save); tok;
	     tok = strtok_r(NULL, ",", &save)) {
		unsigned long weight = 1;
		char *sep = strchr(tok, ':');
		unsigned int i;

		if (sep) {
			char *end;

			*sep++ = '\0';
			weight = strtoul(sep, &end, 10);
			if (end == sep || *end) {
				fprintf(stderr, "Invalid weight: %s\n", sep);
				ret = -EINVAL;
				break;
			}
		}

		for (i = 0; i < NR_OPS; i++)
			if (!strcmp(tok, usemem_ops[i].name))
				break;

		if (i == NR_OPS) {
			fprintf(stderr, "Unknown operation: %s\n", tok);
			ret = -EINVAL;
			break;
		}

		op_weight[i] += weight;
		total_weight += weight;
	}

	free(copy);

	if (!ret && !total_weight) {
		fprintf(stderr, "The operation mix is empty\n");
		ret = -EINVAL;
	}

	return ret;
}

static void print_header(void)
{
	bool first = true;
	unsigned int i;

	if (nr_threads > 1)
		printf("# %u threads on %s each, ", nr_threads, size_str);
	else
		printf("# one thread on %s, ", size_str);

	printf("%s access, ", access_seq ? "sequential" : "random");
	if (thp)
		printf("MADV_%sHUGEPAGE\n", thp > 0 ? "" : "NO");
	else
		printf("no huge page hint\n");

	for (i = 0; i < NR_OPS; i++) {
		if (op_weight[i] && usemem_ops[i].chunked) {
			printf("# chunks of %s, at most %u of the %u in a region "
			       "left unmapped\n", chunk_str, max_holes, nr_chunks);
			break;
		}
	}

	printf("# mix:");
	for (i = 0; i < NR_OPS; i++) {
		if (!op_weight[i])
			continue;
		printf("%s %s %u", first ? "" : ",", usemem_ops[i].name,
		       op_weight[i]);
		first = false;
	}
	printf("\n");
}

static void report(struct usemem_thread *threads, u64 runtime_ns)
{
	double secs = (double)runtime_ns / NSEC_PER_SEC;
	u64 nothing[NR_OPS] = { 0 }, failed[NR_OPS] = { 0 };
	u64 nr_ops = 0, nr_nothing = 0, nr_failed = 0;
	int error[NR_OPS] = { 0 };
	struct hist *total;
	unsigned int i, op;

	total = calloc(NR_OPS, sizeof(*total));
	if (!total)
		return;

	for (i = 0; i < nr_threads; i++) {
		for (op = 0; op < NR_OPS; op++) {
			hist_merge(&total[op], &threads[i].hist[op]);
			nothing[op] += threads[i].nothing[op];
			failed[op] += threads[i].failed[op];
			if (!error[op])
				error[op] = threads[i].error[op];
		}
	}

	for (op = 0; op < NR_OPS; op++) {
		nr_ops += total[op].nr;
		nr_nothing += nothing[op];
		nr_failed += failed[op];
	}

	if (bench_format == BENCH_FORMAT_SIMPLE) {
		printf("%lf\n", nr_ops / secs);
		goto out;
	}

	printf("#\n# %12s %12s %11s %10s %10s %10s %10s %10s %10s\n",
	       "operation", "ops", "ops/sec", "avg", "p50", "p90", "p99",
	       "p99.9", "max");
	printf("# %12s %12s %11s %10s %10s %10s %10s %10s %10s\n",
	       "", "", "", "ns", "ns", "ns", "ns", "ns", "ns");

	for (op = 0; op < NR_OPS; op++) {
		struct hist *h = &total[op];

		if (!op_weight[op])
			continue;

		printf("  %12s %12" PRIu64 " %11.0f %10" PRIu64,
		       usemem_ops[op].name, h->nr, h->nr / secs,
		       h->nr ? h->sum / h->nr : 0);
		printf(" %10" PRIu64 " %10" PRIu64 " %10" PRIu64
		       " %10" PRIu64 " %10" PRIu64 "\n",
		       hist_percentile(h, 50), hist_percentile(h, 90),
		       hist_percentile(h, 99), hist_percentile(h, 99.9),
		       h->max);
	}

	printf("#\n# %" PRIu64 " operations in %.3f secs, %.0f ops/sec\n",
	       nr_ops, secs, nr_ops / secs);

	if (nr_nothing) {
		printf("# nothing to do:");
		for (op = 0; op < NR_OPS; op++)
			if (nothing[op])
				printf(" %s %" PRIu64, usemem_ops[op].name,
				       nothing[op]);
		printf("\n");
	}

	for (op = 0; op < NR_OPS; op++)
		if (failed[op])
			printf("# %s refused %" PRIu64 " times: %s\n",
			       usemem_ops[op].name, failed[op],
			       strerror(error[op]));
out:
	free(total);
}

static void toggle_done(int sig __maybe_unused,
			siginfo_t *info __maybe_unused,
			void *uc __maybe_unused)
{
	done = true;
}

int bench_mem_usemem(int argc, const char **argv)
{
	struct usemem_thread *threads;
	struct sigaction act = { 0 };
	u64 start_ns, runtime_ns;
	unsigned int i, nr_started;
	int ret = 0;
	s64 size;

	argc = parse_options(argc, argv, options, bench_mem_usemem_usage, 0);
	if (argc)
		usage_with_options(bench_mem_usemem_usage, options);

	ret = parse_ops();
	if (ret)
		return ret == -EAGAIN ? 0 : 1;

	if (!nr_threads) {
		fprintf(stderr, "At least one thread, please\n");
		return 1;
	}

	if (!nr_secs && !nr_loops) {
		fprintf(stderr, "Give either a runtime or a loop count\n");
		return 1;
	}

	page_size = sysconf(_SC_PAGESIZE);
	align_size = huge_page_size();

	size = perf_atoll(chunk_str);
	if (size < (s64)page_size) {
		fprintf(stderr, "Invalid chunk: %s\n", chunk_str);
		return 1;
	}
	chunk_size = size & ~(page_size - 1);

	size = perf_atoll(size_str);
	if (size < (s64)chunk_size) {
		fprintf(stderr, "Invalid size: %s\n", size_str);
		return 1;
	}

	/* Whole chunks only, so that every operation covers the same amount */
	nr_chunks = size / chunk_size;
	region_size = (size_t)nr_chunks * chunk_size;

	/* One chunk always stays mapped, so an access always has a page to take */
	max_holes = (unsigned long)nr_chunks * holes_pct / 100;
	if (max_holes >= nr_chunks)
		max_holes = nr_chunks - 1;

	threads = calloc(nr_threads, sizeof(*threads));
	if (!threads)
		return 1;

	if (bench_format == BENCH_FORMAT_DEFAULT)
		print_header();

	for (i = 0; i < nr_threads; i++) {
		struct usemem_thread *t = &threads[i];

		t->nr = i;
		t->nr_pages = region_size / page_size;
		/* Distinct streams, still fixed by the seed */
		t->rnd_state = seed + i * 2654435761UL;
		t->region = alloc_region();
		if (!t->region) {
			fprintf(stderr, "Failed to map %s for thread %u\n",
				size_str, i);
			ret = -ENOMEM;
			goto out;
		}

		t->mapped = malloc(nr_chunks * sizeof(*t->mapped));
		if (!t->mapped) {
			ret = -ENOMEM;
			goto out;
		}
		memset(t->mapped, true, nr_chunks * sizeof(*t->mapped));
	}

	act.sa_flags = SA_SIGINFO;
	act.sa_sigaction = toggle_done;
	if (sigaction(SIGINT, &act, NULL))
		fprintf(stderr, "Cannot install a SIGINT handler\n");

	mutex_init(&start_lock);
	cond_init(&start_parent);
	cond_init(&start_worker);
	threads_starting = nr_threads;

	for (i = 0; i < nr_threads; i++) {
		if (pthread_create(&threads[i].thread, NULL, worker_thread,
				   &threads[i])) {
			fprintf(stderr, "Failed to start thread %u\n", i);
			done = true;
			ret = -EAGAIN;
			break;
		}
	}
	nr_started = i;

	mutex_lock(&start_lock);
	/* A thread that was never started will not count itself in */
	threads_starting -= nr_threads - nr_started;
	while (threads_starting)
		cond_wait(&start_parent, &start_lock);
	start_ns = now_ns();
	cond_broadcast(&start_worker);
	mutex_unlock(&start_lock);

	if (nr_secs) {
		sleep(nr_secs);
		done = true;
	}

	for (i = 0; i < nr_started; i++)
		pthread_join(threads[i].thread, NULL);
	runtime_ns = now_ns() - start_ns;

	cond_destroy(&start_parent);
	cond_destroy(&start_worker);
	mutex_destroy(&start_lock);

	if (!ret)
		report(threads, runtime_ns);
out:
	for (i = 0; i < nr_threads; i++) {
		if (threads[i].region)
			munmap(threads[i].region, region_size);
		free(threads[i].mapped);
	}
	free(threads);

	return ret ? 1 : 0;
}
