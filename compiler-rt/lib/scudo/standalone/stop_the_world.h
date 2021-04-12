#ifndef __STOP_WORLD_H__
#define __STOP_WORLD_H__

#include "atomic_helpers.h"
#include "fcntl.h"
#include "internal_defs.h"
#include "mutex.h"
#include <signal.h>
#include "sys/mman.h"
#include "pthread.h"
#include <ucontext.h>
#include <unistd.h>

#include "errno.h"
#include "stdio.h"

#define MAX_THREADS 100
#define MAX_DIRTY_PAGES 10000

namespace scudo {

class StopTheWorldBase {
protected:
  atomic_u32 threadCount;
  pthread_t threads[MAX_THREADS];
  HybridMutex threadsLock;

  atomic_u64 StopCount;
  atomic_u32 StoppedThreads;

  uptr PageMask;

  static constexpr size_t BUFF_SIZE = 256;
  uint64_t pagemapBuff[BUFF_SIZE];
  size_t buffBeg;
  int pagemapFd;
  off_t pagemapOff;

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
  void addThread(pthread_t thd);

  bool isPageDirty(uptr page);

  void init();
};

template <typename AllocatorT>
class StopTheWorld : public StopTheWorldBase {
  AllocatorT *Allocator;
public:
  void init(AllocatorT *_allocator);
  void protectHeap();
  void stop();
  void resume();
};

template<typename AllocatorT> void StopTheWorld<AllocatorT>::init(AllocatorT *_allocator) {
  StopTheWorldBase::init();

  Allocator = _allocator;
  threadsLock.init();
}

template<typename AllocatorT>
void StopTheWorld<AllocatorT>::protectHeap() {
  int fd = open("/proc/self/clear_refs", O_WRONLY);
  if (fd < 0) printf("%d\n", errno);
  CHECK(fd >= 0);
  write(fd, (const void*)"4", 1);
  close(fd);
}

template<typename AllocatorT>
void StopTheWorld<AllocatorT>::stop() {
//  outputRaw("stop()\n");
  pagemapFd = open("/proc/self/pagemap", O_RDONLY);
  pagemapOff = 0;
  buffBeg = 0;
  CHECK(pagemapFd >= 0);

  // Acquire mutexes for iteration. Do it now, as doing so after
  // suspending other threads could lead to deadlock.
  Allocator->disable();

  uint32_t tc = atomic_load(&threadCount, memory_order_acquire);

  atomic_fetch_add(&StopCount, 1, memory_order_seq_cst);
  for (uint32_t i = 0; i < tc; i++)
    CHECK(!pthread_kill(threads[i], SIG_STOP_WORLD));

  while (atomic_load(&StoppedThreads, memory_order_acquire) < tc)
    pthread_yield();
}

template<typename AllocatorT>
void StopTheWorld<AllocatorT>::resume() {
//  outputRaw("resume()\n");
  Allocator->enable();


  uint32_t tc = atomic_load(&threadCount, memory_order_acquire);

  atomic_fetch_add(&StopCount, 1, memory_order_seq_cst);
  for (uint32_t i = 0; i < tc; i++)
    CHECK(!pthread_kill(threads[i], SIG_RESUME_WORLD));

  atomic_store(&StoppedThreads, 0, memory_order_relaxed);
  close(pagemapFd);
}

inline bool StopTheWorldBase::isPageDirty(uptr page) {
  page /= getPageSizeCached();

  if (page - buffBeg <= BUFF_SIZE)
    return (pagemapBuff[page - buffBeg] >> 55) & 1; // Read soft-dirty bit

  off_t offset = page * 8; // 8 Bytes per page in map

  if (offset != pagemapOff) {
    pagemapOff = lseek(pagemapFd, offset, SEEK_SET);
    CHECK_EQ(pagemapOff, offset);
  }

  CHECK_EQ(read(pagemapFd, (void*)pagemapBuff, BUFF_SIZE*8), BUFF_SIZE*8);
  buffBeg = page;
  return (pagemapBuff[0] >> 55) & 1; // Read soft-dirty bit
}

};

#endif // __STOP_WORLD_H__
