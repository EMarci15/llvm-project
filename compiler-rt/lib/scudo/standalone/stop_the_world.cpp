#include "stop_the_world.h"

namespace scudo {

StopTheWorldBase *StopTheWorldBase::_Instance;

NOINLINE static void save_caller_regs() { asm(""); }

extern "C" {
static void write_fault_handler(int sig, siginfo_t *si, void *raw_sc) {
  uptr addr = (uptr)si->si_addr;
  StopTheWorldBase::Instance()->sigsegv(addr);
}

static void suspend_handler(int sig, siginfo_t *si, void *raw_sc) {
  StopTheWorldBase::Instance()->sigsus();
}

static void resume_handler(int sig, siginfo_t *si, void *raw_sc) {
  // Do nothing, resumes suspend_handler on sigsuspend
}
};
  
void StopTheWorldBase::init() {
  StopTheWorldBase *t = this;
  __atomic_store(&_Instance, &t, memory_order_relaxed);
  PageMask = ~(getPageSizeCached()-1);

  struct sigaction act, oldact;
  act.sa_flags = SA_RESTART | SA_SIGINFO;
  act.sa_sigaction = write_fault_handler;
  CHECK(!sigemptyset(&act.sa_mask));
  CHECK(!sigaddset(&act.sa_mask, SIG_STOP_WORLD));

  CHECK(!sigaction(SIGSEGV, &act, &oldact));
  // XXX ignoring old handler; this will break programs with handlers

  CHECK(!sigemptyset(&suspend_wait_mask));
  CHECK(!sigaddset(&suspend_wait_mask, SIG_RESUME_WORLD));

  CHECK(!sigfillset(&act.sa_mask));
  CHECK(!sigdelset(&act.sa_mask, SIGINT));
  CHECK(!sigdelset(&act.sa_mask, SIGQUIT));
  CHECK(!sigdelset(&act.sa_mask, SIGABRT));
  CHECK(!sigdelset(&act.sa_mask, SIGTERM));

  act.sa_sigaction = suspend_handler;
  CHECK(!sigaction(SIG_STOP_WORLD, &act, &oldact));

  act.sa_sigaction = resume_handler;
  CHECK(!sigaction(SIG_RESUME_WORLD, &act, &oldact));
}

inline void StopTheWorldBase::sigsegv(uptr addr) {
  uptr page_addr = addr & PageMask;
  bool in_allocd_block = allocd(page_addr);

  CHECK(!in_allocd_block);

  UNPROTECT(page_addr, getPageSizeCached());
  addDirtyPage(page_addr);
}

inline void StopTheWorldBase::sigsus() {
  ucontext_t ctx;
  CHECK(!getcontext(&ctx));
  save_caller_regs();

  uint64_t sc = atomic_load_relaxed(&StopCount);
  CHECK(sc % 2);

  do {
    atomic_fetch_add(&StoppedThreads, 1, memory_order_acq_rel);
    sigsuspend(&suspend_wait_mask);
    DCHECK(errno == EINTR);
    sc = atomic_load_relaxed(&StopCount);
  } while (sc % 2);
}

void StopTheWorldBase::addDirtyPage(uptr addr) {
  uint32_t ind = atomic_fetch_add(&dirtyPageCount, (u32)1,
                                              memory_order_relaxed);
  dirtyPages[ind] = addr;
}

};
