// SPDX-License-Identifier: GPL-2.0

#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <x86intrin.h>

#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/shm.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include "../kselftest.h" /* For __cpuid_count() */

#define XSAVE_HDR_OFFSET	512
#define XSAVE_HDR_SIZE		64

struct xsave_buffer {
	union {
		struct {
			char legacy[XSAVE_HDR_OFFSET];
			char header[XSAVE_HDR_SIZE];
			char extended[0];
		};
		char bytes[0];
	};
};

#ifdef __x86_64__
#define REX_PREFIX	"0x48, "
#else
#define REX_PREFIX
#endif

#define XSAVE		".byte " REX_PREFIX "0x0f,0xae,0x27"
#define XSAVEC		".byte " REX_PREFIX "0x0f,0xc7,0x27"
#define XRSTOR		".byte " REX_PREFIX "0x0f,0xae,0x2f"

static inline void xsave(struct xsave_buffer *xbuf, uint64_t rfbm)
{
	uint32_t rfbm_lo = rfbm;
	uint32_t rfbm_hi = rfbm >> 32;

	asm volatile(XSAVE
		     : : "D" (xbuf), "a" (rfbm_lo), "d" (rfbm_hi)
		     : "memory");
}

static inline void xrstor(struct xsave_buffer *xbuf, uint64_t rfbm)
{
	uint32_t rfbm_lo = rfbm;
	uint32_t rfbm_hi = rfbm >> 32;

	asm volatile(XRSTOR
		     : : "D" (xbuf), "a" (rfbm_lo), "d" (rfbm_hi));
}

/* err() exits and will not return */
#define fatal_error(msg, ...)   err(1, "[FAIL]\t" msg, ##__VA_ARGS__)

#define XFEATURE_APX		19
#define XFEATURE_MASK_APX	(1 << XFEATURE_APX)

#define CPUID_LEAF1_ECX_XSAVE_MASK	(1 << 26)
#define CPUID_LEAF1_ECX_OSXSAVE_MASK	(1 << 27)
static inline void check_cpuid_xsave(void)
{
	uint32_t eax, ebx, ecx, edx;

	/*
	 * CPUID.1:ECX.XSAVE[bit 26] enumerates general
	 * support for the XSAVE feature set, including
	 * XGETBV.
	 */
	__cpuid_count(1, 0, eax, ebx, ecx, edx);
	if (!(ecx & CPUID_LEAF1_ECX_XSAVE_MASK))
		fatal_error("cpuid: no CPU xsave support");
	if (!(ecx & CPUID_LEAF1_ECX_OSXSAVE_MASK))
		fatal_error("cpuid: no OS xsave support");
}

static uint32_t xbuf_size;

static struct {
	uint32_t xbuf_offset;
	uint32_t size;
} apx_info;

#define CPUID_LEAF_XSTATE		0xd
#define CPUID_SUBLEAF_XSTATE_USER	0x0

static void check_cpuid_apx(void)
{
	uint32_t eax, ebx, ecx, edx;

	__cpuid_count(CPUID_LEAF_XSTATE, CPUID_SUBLEAF_XSTATE_USER,
		      eax, ebx, ecx, edx);

	/*
	 * EBX enumerates the size (in bytes) required by the XSAVE
	 * instruction for an XSAVE area containing all the user state
	 * components corresponding to bits currently set in XCR0.
	 *
	 * Stash that off so it can be used to allocate buffers later.
	 */
	xbuf_size = ebx;

	__cpuid_count(CPUID_LEAF_XSTATE, XFEATURE_APX,
		      eax, ebx, ecx, edx);
	/*
	 * eax: APX state component size
	 * ebx: APX state component offset in user buffer
	 */
	if (!eax || !ebx)
		fatal_error("xstate cpuid: invalid apx size/offset: %d/%d",
				eax, ebx);

	apx_info.size = eax;
	apx_info.xbuf_offset = ebx;
	printf("[INFO]\toffset = %u and size = %u\n",
		apx_info.xbuf_offset, apx_info.size);
}

/* The helpers for managing XSAVE buffer and states: */

struct xsave_buffer *alloc_xbuf(void)
{
	struct xsave_buffer *xbuf;

	/* XSAVE buffer should be 64B-aligned. */
	xbuf = aligned_alloc(64, xbuf_size);
	if (!xbuf)
		fatal_error("aligned_alloc()");
	return xbuf;
}

static inline void clear_xstate_header(struct xsave_buffer *buffer)
{
	memset(&buffer->header, 0, sizeof(buffer->header));
}

static inline void set_xstatebv(struct xsave_buffer *buffer, uint64_t bv)
{
	/* XSTATE_BV is at the beginning of the header: */
	*(uint64_t *)(&buffer->header) = bv;
}

static void set_rand_apx(struct xsave_buffer *xbuf)
{
	int *ptr = (int *)&xbuf->bytes[apx_info.xbuf_offset];
	int data;
	int i;

	/*
	 * Ensure that 'data' is never 0.  This ensures that
	 * the registers are never in their initial configuration
	 * and thus never tracked as being in the init state.
	 */
	data = rand() | 1;

	for (i = 0; i < apx_info.size / sizeof(int); i++, ptr++)
		*ptr = data;
}

struct xsave_buffer *stashed_xsave;

static void init_stashed_xsave(void)
{
	stashed_xsave = alloc_xbuf();
	if (!stashed_xsave)
		fatal_error("failed to allocate stashed_xsave\n");
	clear_xstate_header(stashed_xsave);
}

static void free_stashed_xsave(void)
{
	free(stashed_xsave);
}

/* Work around printf() being unsafe in signals: */
#define SIGNAL_BUF_LEN 1000
char signal_message_buffer[SIGNAL_BUF_LEN];
void sig_print(char *msg)
{
	int left = SIGNAL_BUF_LEN - strlen(signal_message_buffer) - 1;

	strncat(signal_message_buffer, msg, left);
}

/*
 * Use XRSTOR to populate the APX registers with random data.
 *
 * Return true if successful; otherwise, false.
 */
static inline void load_rand_apx(struct xsave_buffer *xbuf)
{
	clear_xstate_header(xbuf);
	set_xstatebv(xbuf, XFEATURE_MASK_APX);
	set_rand_apx(xbuf);
	xrstor(xbuf, XFEATURE_MASK_APX);
}

/*
 * Save current register state and compare it to @xbuf1.'
 *
 * Returns false if @xbuf1 matches the registers.
 * Returns true  if @xbuf1 differs from the registers.
 */
static inline bool validate_apx_regs(struct xsave_buffer *xbuf1)
{
	struct xsave_buffer *xbuf2;
	int ret;

	xbuf2 = alloc_xbuf();
	if (!xbuf2)
		fatal_error("failed to allocate XSAVE buffer\n");

	xsave(xbuf2, XFEATURE_MASK_APX);
	ret = memcmp(&xbuf1->bytes[apx_info.xbuf_offset],
		     &xbuf2->bytes[apx_info.xbuf_offset],
		     apx_info.size);

	free(xbuf2);

	if (ret == 0)
		return false;
	return true;
}

/* Context switching test */

static struct _ctxtswtest_cfg {
	unsigned int iterations;
	unsigned int num_threads;
} ctxtswtest_config;

struct futex_info {
	pthread_t thread;
	int nr;
	pthread_mutex_t mutex;
	struct futex_info *next;
};

static void *check_apx(void *info)
{
	struct futex_info *finfo = (struct futex_info *)info;
	struct xsave_buffer *xbuf;
	int i;

	xbuf = alloc_xbuf();
	if (!xbuf)
		fatal_error("unable to allocate XSAVE buffer");

	/*
	 * Load random data into 'xbuf' and then restore
	 * it to the apx registers themselves.
	 */
	load_rand_apx(xbuf);
	for (i = 0; i < ctxtswtest_config.iterations; i++) {
		pthread_mutex_lock(&finfo->mutex);

		/*
		 * Ensure the register values have not
		 * diverged from those recorded in 'xbuf'.
		 */
		if (validate_apx_regs(xbuf)) {
			printf("i = %d, nr = %d\n", i, finfo->nr);
			fatal_error("APX registers changed");
		}

		/* Load new, random values into xbuf and registers */
		load_rand_apx(xbuf);

		/*
		 * The last thread's last unlock will be for
		 * thread 0's mutex.  However, thread 0 will
		 * have already exited the loop and the mutex
		 * will already be unlocked.
		 *
		 * Because this is not an ERRORCHECK mutex,
		 * that inconsistency will be silently ignored.
		 */
		pthread_mutex_unlock(&finfo->next->mutex);
	}

	free(xbuf);
	/*
	 * Return this thread's finfo, which is
	 * a unique value for this thread.
	 */
	return finfo;
}

static int create_threads(int num, struct futex_info *finfo)
{
	int i;

	for (i = 0; i < num; i++) {
		int next_nr;

		finfo[i].nr = i;
		/*
		 * Thread 'i' will wait on this mutex to
		 * be unlocked.  Lock it immediately after
		 * initialization:
		 */
		pthread_mutex_init(&finfo[i].mutex, NULL);
		pthread_mutex_lock(&finfo[i].mutex);

		next_nr = (i + 1) % num;
		finfo[i].next = &finfo[next_nr];

		if (pthread_create(&finfo[i].thread, NULL, check_apx, &finfo[i]))
			fatal_error("pthread_create()");
	}
	return 0;
}

static void affinitize_cpu0(void)
{
	cpu_set_t cpuset;

	CPU_ZERO(&cpuset);
	CPU_SET(0, &cpuset);

	if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0)
		fatal_error("sched_setaffinity to CPU 0");
}

static void test_context_switch(void)
{
	struct futex_info *finfo;
	int i;

	/* Affinitize to one CPU to force context switches */
	affinitize_cpu0();

	printf("[RUN]\tCheck apx context switches, %d iterations, %d threads.\n",
	       ctxtswtest_config.iterations,
	       ctxtswtest_config.num_threads);


	finfo = malloc(sizeof(*finfo) * ctxtswtest_config.num_threads);
	if (!finfo)
		fatal_error("malloc()");

	create_threads(ctxtswtest_config.num_threads, finfo);

	/*
	 * This thread wakes up thread 0
	 * Thread 0 will wake up 1
	 * Thread 1 will wake up 2
	 * ...
	 * the last thread will wake up 0
	 *
	 * ... this will repeat for the configured
	 * number of iterations.
	 */
	pthread_mutex_unlock(&finfo[0].mutex);

	/* Wait for all the threads to finish: */
	for (i = 0; i < ctxtswtest_config.num_threads; i++) {
		void *thread_retval;
		int rc;

		rc = pthread_join(finfo[i].thread, &thread_retval);

		if (rc)
			fatal_error("pthread_join() failed for thread %d err: %d\n",
					i, rc);

		if (thread_retval != &finfo[i])
			fatal_error("unexpected thread retval for thread %d: %p\n",
					i, thread_retval);

	}

	printf("[OK]\tNo incorrect case was found.\n");

	free(finfo);
}

/* Signal return test */

static void sethandler(int sig, void (*handler)(int, siginfo_t *, void *),
		       int flags)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = handler;
	sa.sa_flags = SA_SIGINFO | flags;
	sigemptyset(&sa.sa_mask);
	if (sigaction(sig, &sa, 0))
		err(1, "sigaction");
}

static void clearhandler(int sig)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_DFL;
	sigemptyset(&sa.sa_mask);
	if (sigaction(sig, &sa, 0))
		err(1, "sigaction");
}

static void handle_signal(int sig, siginfo_t *info, void *ctx_void)
{
	load_rand_apx(stashed_xsave);
}

static void test_signal(void)
{
	struct xsave_buffer *xbuf;

	xbuf = alloc_xbuf();
	if (!xbuf)
		fatal_error("unable to allocate XSAVE buffer");

	printf("[RUN]\tCheck APX state restoration with signal.\n");

	sethandler(SIGALRM, handle_signal, 0);

	load_rand_apx(xbuf);

	raise(SIGALRM);

	if (validate_apx_regs(xbuf))
		fatal_error("APX registers changed from signal");

	printf("[OK]\tThe APX state was retained on the sig return.\n");
	clearhandler(SIGALRM);
}

/* Ptrace test */

static int inject_apx(pid_t target)
{
	struct xsave_buffer *xbuf;
	struct iovec iov;
	int ret;

	xbuf = alloc_xbuf();
	if (!xbuf)
		fatal_error("unable to allocate XSAVE buffer");

	iov.iov_base = xbuf;
	iov.iov_len = xbuf_size;

	load_rand_apx(xbuf);
	memcpy(stashed_xsave, xbuf, xbuf_size);

	if (ptrace(PTRACE_SETREGSET, target, (uint32_t)NT_X86_XSTATE, &iov)) {
		if (errno != EFAULT)
			err(1, "PTRACE_SETREGSET");
		else
			return errno;
	}

	if (ptrace(PTRACE_GETREGSET, target, (uint32_t)NT_X86_XSTATE, &iov))
		err(1, "PTRACE_GETREGSET");

	ret = memcmp(&xbuf->bytes[apx_info.xbuf_offset],
		     &stashed_xsave->bytes[apx_info.xbuf_offset],
		     apx_info.size);
	if (!ret)
		return 0;
	else
		return -1;
}

static void test_ptrace(void)
{
	int status, rc;
	pid_t child;

	child = fork();
	if (child < 0) {
		err(1, "fork");
	} else if (!child) {
		if (ptrace(PTRACE_TRACEME, 0, NULL, NULL))
			err(1, "PTRACE_TRACEME");
		raise(SIGTRAP);
		_exit(0);
	}

	do {
		wait(&status);
	} while (WSTOPSIG(status) != SIGTRAP);

	printf("[RUN]\tCheck APX state injection via ptrace.\n");

	rc = inject_apx(child);
	if (!rc)
		printf("[OK]\tAPX state was written on ptracee.\n");
	else
		printf("[FAIL]\tAPX state was not written on ptracee\n");

	ptrace(PTRACE_DETACH, child, NULL, NULL);
	wait(&status);
	if (!WIFEXITED(status) || WEXITSTATUS(status))
		err(1, "ptrace test");
}

int main(void)
{
	/* Check hardware availability at first */
	check_cpuid_xsave();
	check_cpuid_apx();

	init_stashed_xsave();

	ctxtswtest_config.iterations = 10;
	ctxtswtest_config.num_threads = 5;
	test_context_switch();

	test_signal();

	test_ptrace();

	free_stashed_xsave();

	return 0;
}
