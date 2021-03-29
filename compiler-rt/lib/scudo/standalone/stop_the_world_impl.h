#ifndef __STOP_WORLD_IMPL_H
#define __STOP_WORLD_IMPL_H

#include "combined.h"
#include "stop_the_world.h"

/*
* This implementation maintains dirty bits itself by catching write
* faults and keeping track of them.  We assume nobody else catches
* SIGBUS or SIGSEGV.  We assume no write faults occur in system calls.
* This means that clients must ensure that system calls don't write
* to the write-protected heap.  Probably the best way to do this is to
* ensure that system calls write at most to pointer-free objects in the
* heap, and do even that only if we are on a platform on which those
* are not protected.  Another alternative is to wrap system calls
* (see example for read below), but the current implementation holds
* applications.
* We assume the page size is a multiple of HBLKSIZE.
* We prefer them to be the same.  We avoid protecting pointer-free
* objects only if they are the same.
*/

#include <sys/mman.h>
#include <signal.h>
#include <sys/syscall.h>

static void PROTECT(uptr addr, uptr len) {
  if (!mprotect((void*)(addr), (size_t)(len), PROT_READ) < 0)
		abort("mprotect failed");
}
static void UNPROTECT(uptr addr, uptr len) \
  if (mprotect((void*)(addr), (size_t)(len), (PROT_READ | PROT_WRITE)) < 0)
	  abort("un-mprotect failed");
}

# if defined(__GLIBC__)
#   if __GLIBC__ < 2 || __GLIBC__ == 2 && __GLIBC_MINOR__ < 2
#       error glibc too old?
#   endif
# endif

#include <errno.h>
#include <ucontext.h>

static void write_fault_handler(int sig, siginfo_t *si, void *raw_sc)
{
	void *addr = (void*)si->si_addr;

  if (sig == SIGSEGV) {
		bool in_allocd_block = StopTheWorld::Instance->allocd(addr);

    if (!in_allocd_block)
			abort("Uncaught SIGSEGV.");

		UNPROTECT(addr, getPageSizeCached());
		uint32_t ind = atomic_fetch_add(dirtyPageCount, 1, memory_order_relaxed);
	  dirtyPages[ind] = addr;
	}

	abort("Uncaught SIGBUS or other.");
}

static NOINLINE void save_caller_regs() { asm(""); }

static constexpr SIG_STOP_WORLD = SIGUSR1;
static constexpr SIG_RESUME_WORLD = SIGUSR2;
static atomic_u64 StopCount;
static atomic_u32 StoppedThreads;

static struct sigaction suspend_wait_mask;

static void suspend_handler(int sig, siginfo_t *si, void *raw_sc) {
	ucontext_t ctx;
	getcontext(&ctx);
	save_caller_regs();

	uint64_t sc = atomic_load_relaxed(&StopCount);
	assert(sc % 2);

	do {
		atomic_fetch_add(&StoppedThreads, 1, memory_order_acq_rel);
		sigsuspend(&suspend_wait_mask);
		sc = atomic_load_relaxed(&StopCount);
	} while (sc % 2);
}

static void resume_handler(int sig, siginfo_t *si, void *raw_sc) {
	// Do nothing, resumes suspend_handler on sigsuspend
}

template<typename AllocatorT>
void StopTheWorld<AllocatorT>::init(AllocatorT *_allocator) {
	Allocator = _allocator;
	threadsLock.init();

	struct sigaction act;
	act.sa_flags = SA_RESTART | SA_SIGINFO;
	act.sa_sigaction = write_fault_handler;
	sigemptyset(&act.sa_mask);
	sigaddset(&act.sa_mask, SIG_STOP_WORLD);

	sigaction(SIGSEGV, &act, 0);
	// XXX ignoring old handler; this will break programs with handlers

	sigemptyset(&suspend_wait_mask);
	sigaddset(&suspend_wait_mask, SIG_RESUME_WORLD);

	sigfillset(&act.sa_mask);
	sigdelset(&act.sa_mask, SIGINT);
	sigdelset(&act.sa_mask, SIGQUIT);
	sigdelset(&act.sa_mask, SIGABRT);
	sigdelset(&act.sa_mask, SIGTERM);

	act.sa_sigaction = suspend_handler;
	sigaction(SIG_STOP_WORLD, suspend_handler);

	act.sa_sigaction = resume_handler;
	sigaction(SIG_RESUME_WORLD, resume_handler);
}

template<typename AllocatorT>
void StopTheWorld<AllocatorT>::addThread(pthread_t thd) {
	ScopedLock L(threadsLock);

	threads[threadCount] = thd;
	atomic_fetch_add(&threadCount, 1, memory_order_release);
}

template<typename AllocatorT>
void StopTheWorld<AllocatorT>::protectHeap() {
	atomic_store(&dirtyPagesCount, 0, memory_order_seq_cst);
	Allocator->iterateOverRegions(PROTECT);
}

template<typename AllocatorT>
void StopTheWorld<AllocatorT>::stop() {
	uint32_t tc = atomic_load(&threadCount, memory_order_acquire);

	atomic_fetch_add(&StopCount, 1, memory_order_seq_cst);
	for (uint32_t i = 0; i < tc; i++)
		pthread_kill(threads[i], SIG_STOP_WORLD);

	while (atomic_load(&StoppedThreads, memory_order_acquire) < tc)
		pthread_yield();
}

template<typename AllocatorT>
void StopTheWorld<AllocatorT>::resume() {
	uint32_t tc = atomic_load(&threadCount, memory_order_acquire);

	atomic_fetch_add(&StopCount, 1, memory_order_seq_cst);
	for (uint32_t i = 0; i < tc; i++)
		pthread_kill(threads[i], SIG_RESUME_WORLD);

	atomic_store(&StoppedThreads, 0, memory_order_relaxed);
}

template<typename AllocatorT, typename f>
void StopTheWorld<AllocatorT>::iterateOverDirtyAtomic(f Callback) {
	stop();
	uint32_t N = atomic_load_relaxed(&dirtyPageCount);
	for (uint32_t i = 0; i < N; i++)
		Callback(dirtyPages[i], getPageSizeCached);
	resume();
}

};

#endif
