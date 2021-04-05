#ifndef __STOP_WORLD_H__
#define __STOP_WORLD_H__

#include "atomic_helpers.h"
#include "internal_defs.h"
#include "mutex.h"
#include <signal.h>
#include "sys/mman.h"
#include "pthread.h"
#include <ucontext.h>

#define MAX_THREADS 100
#define MAX_DIRTY_PAGES 10000

namespace scudo {

class StopTheWorldBase {
protected:
  atomic_u32 threadCount;
  pthread_t threads[MAX_THREADS];
  HybridMutex threadsLock;

  atomic_u32 dirtyPageCount;
  uptr dirtyPages[MAX_DIRTY_PAGES];

  atomic_u64 StopCount;
  atomic_u32 StoppedThreads;

  uptr PageMask;

  static constexpr int SIG_STOP_WORLD = SIGUSR1;
  static constexpr int SIG_RESUME_WORLD = SIGUSR2;

  static StopTheWorldBase *_Instance;

  sigset_t suspend_wait_mask;
public:
  inline static StopTheWorldBase *Instance() {
    // atomic load for signal safety
    StopTheWorldBase *instance;
    __atomic_load(&_Instance, &instance, memory_order_relaxed);
    return instance;
  }

  void sigsus();
  void addDirtyPage(uptr addr);
  void addThread(pthread_t thd);

  virtual bool allocd(uptr p) = 0;

  void init();
};

template <typename AllocatorT>
class StopTheWorld : public StopTheWorldBase {
  AllocatorT *Allocator;
public:
  void init(AllocatorT *_allocator);
  void protectHeap();
  void unprotectHeap();
  void stop();
  void resume();
  template<typename f> void iterateOverDirtyAtomic(f Callback);
  bool allocd(uptr p);
};

template<typename AllocatorT> template<typename f>
void StopTheWorld<AllocatorT>::iterateOverDirtyAtomic(f Callback) {
  stop();
  uint32_t N = atomic_load_relaxed(&dirtyPageCount);
  for (uint32_t i = 0; i < N; i++)
    Callback(dirtyPages[i], getPageSizeCached());
  resume();
}

template<typename AllocatorT> void StopTheWorld<AllocatorT>::init(AllocatorT *_allocator) {
  StopTheWorldBase::init();

  Allocator = _allocator;
  threadsLock.init();
}

template<typename AllocatorT>
bool StopTheWorld<AllocatorT>::allocd(uptr ptr) {
  return Allocator->allocd(ptr);
}

template<typename AllocatorT>
void StopTheWorld<AllocatorT>::protectHeap() {
  // TODO
}

template<typename AllocatorT>
void StopTheWorld<AllocatorT>::unprotectHeap() {
  // TODO
  atomic_store(&dirtyPageCount, 0, memory_order_release);
}

template<typename AllocatorT>
void StopTheWorld<AllocatorT>::stop() {
  // outputRaw("stop()\n");
  uint32_t tc = atomic_load(&threadCount, memory_order_acquire);

  atomic_fetch_add(&StopCount, 1, memory_order_seq_cst);
  for (uint32_t i = 0; i < tc; i++)
    CHECK(!pthread_kill(threads[i], SIG_STOP_WORLD));

  while (atomic_load(&StoppedThreads, memory_order_acquire) < tc)
    pthread_yield();
}

template<typename AllocatorT>
void StopTheWorld<AllocatorT>::resume() {
  // outputRaw("resume()\n");
  uint32_t tc = atomic_load(&threadCount, memory_order_acquire);

  atomic_fetch_add(&StopCount, 1, memory_order_seq_cst);
  for (uint32_t i = 0; i < tc; i++)
    CHECK(!pthread_kill(threads[i], SIG_RESUME_WORLD));

  atomic_store(&StoppedThreads, 0, memory_order_relaxed);
}

};

#endif // __STOP_WORLD_H__
