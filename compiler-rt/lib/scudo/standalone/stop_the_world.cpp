#include "stop_the_world.h"

namespace scudo {

StopTheWorldBase *StopTheWorldBase::_Instance;

NOINLINE static void save_caller_regs() { asm(""); }

extern "C" {
static void suspend_handler(int sig, siginfo_t *si, void *raw_sc) {
  //outputRaw("suspend_handler()\n");
  StopTheWorldBase::Instance()->sigsus();
}

static void resume_handler(int sig, siginfo_t *si, void *raw_sc) {
  //outputRaw("resume_handler()\n");
  // Do nothing, resumes suspend_handler on sigsuspend
}
};
  
void StopTheWorldBase::init() {
  StopTheWorldBase *t = this;
  __atomic_store(&_Instance, &t, memory_order_relaxed);
  PageMask = ~(getPageSizeCached()-1);

  CHECK(!sigfillset(&suspend_wait_mask));
  CHECK(!sigdelset(&suspend_wait_mask, SIG_RESUME_WORLD));
}

void StopTheWorldBase::addThread(pthread_t thd) {
  { ScopedLock L(threadsLock);
    threads[atomic_load_relaxed(&threadCount)] = thd;
    atomic_fetch_add(&threadCount, 1, memory_order_release);
  }

  // Set up handlers
  struct sigaction act, oldact;
  act.sa_flags = SA_RESTART | SA_SIGINFO;

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
