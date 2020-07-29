//===-- quarantine.h --------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef SCUDO_QUARANTINE_H_
#define SCUDO_QUARANTINE_H_

#include "bitvector.h"
#include "list.h"
#include "mutex.h"
#include "string_utils.h"
#include "pthread.h"

namespace scudo {

#define MIN_HEAP_ADDR 0
#define MAX_HEAP_ADDR (((uptr)1)<<47)

struct AddrLimits {
  uptr MinAddr, MaxAddr;

  AddrLimits(): MinAddr(MAX_HEAP_ADDR), MaxAddr(MIN_HEAP_ADDR) {}
  AddrLimits(uptr MinAddr, uptr MaxAddr): MinAddr(MinAddr), MaxAddr(MaxAddr) {}
  AddrLimits(const AddrLimits& other): MinAddr(other.MinAddr), MaxAddr(other.MaxAddr) {}

  inline void combine(const AddrLimits& other) {
    if (other.MinAddr < MinAddr) MinAddr = other.MinAddr;
    if (other.MaxAddr < MaxAddr) MaxAddr = other.MaxAddr;
  }
  inline bool contains(const uptr Ptr) const { return ((Ptr >= MinAddr) && (Ptr < MaxAddr)); }
};

struct QuarantineBatch {
  // With the following count, a batch (and the header that protects it) occupy
  // 4096 bytes on 32-bit platforms, and 8192 bytes on 64-bit.
  static const u32 MaxCount = 509;
  QuarantineBatch *Next;
  uptr Size;
  u32 Count;
  void *Ptrs[MaxCount];
  uptr Sizes[MaxCount];

  static QuarantineBatch *getNewInstance(void *Ptr, uptr Size) {
    const static uptr BatchAllocSize = roundUpTo(sizeof(QuarantineBatch), getPageSizeCached());
    // TODO(marton) Should we cache these?
    QuarantineBatch *Instance =
          (QuarantineBatch*)map(nullptr, BatchAllocSize, "scudo:quarantine");
    DCHECK(Instance);
    
    Instance->init(Ptr, Size);
    return Instance;
  }

  static void deallocate(QuarantineBatch *Instance) {
    const static uptr BatchAllocSize = roundUpTo(sizeof(QuarantineBatch), getPageSizeCached());
    unmap((void*)Instance, BatchAllocSize);
  }

  void init(void *Ptr, uptr Size) {
    Count = 1;
    Ptrs[0] = Ptr;
    Sizes[0] = Size;
    this->Size = Size + sizeof(QuarantineBatch); // Account for the Batch Size.
  }

  // The total size of quarantined nodes recorded in this batch.
  uptr getQuarantinedSize() const { return Size - sizeof(QuarantineBatch); }

  void push_back(void *Ptr, uptr Size) {
    DCHECK_LT(Count, MaxCount);
    Ptrs[Count] = Ptr;
    Sizes[Count++] = Size;
    this->Size += Size;
  }

  AddrLimits addrLimits() const {
    AddrLimits Result;
    for (uptr I = 0; I < Count; ++I)
      Result.combine(AddrLimits((uptr)Ptrs[I], ((uptr)Ptrs[I])+Sizes[I]));
    return Result;
  }

  bool canMerge(const QuarantineBatch *const From) const {
    return Count + From->Count <= MaxCount;
  }

  void merge(QuarantineBatch *const From) {
    DCHECK_LE(Count + From->Count, MaxCount);
    DCHECK_GE(Size, sizeof(QuarantineBatch));

    for (uptr I = 0; I < From->Count; ++I) {
      Ptrs[Count + I] = From->Ptrs[I];
      Sizes[Count + I] = From->Sizes[I];
    }
    Count += From->Count;
    Size += From->getQuarantinedSize();

    From->Count = 0;
    From->Size = sizeof(QuarantineBatch);
  }
};

static_assert(sizeof(QuarantineBatch) <= (1U << 13), ""); // 8Kb.

// Per-thread cache of memory blocks.
class QuarantineCache {
public:
  void initLinkerInitialized() {}
  void init() {
    memset(this, 0, sizeof(*this));
    initLinkerInitialized();
  }

  // Total memory used, including internal accounting.
  uptr getSize() const { return atomic_load_relaxed(&Size); }
  // Memory used for internal accounting.
  uptr getOverheadSize() const { return List.size() * sizeof(QuarantineBatch); }

  void enqueue(void *Ptr, uptr Size) {
    if (List.empty() || List.back()->Count == QuarantineBatch::MaxCount) {
      enqueueBatch(QuarantineBatch::getNewInstance(Ptr, Size));
    } else {
      List.back()->push_back(Ptr, Size);
      addToSize(Size);
    }
  }

  void transfer(QuarantineCache *From) {
    List.append_back(&From->List);
    addToSize(From->getSize());
    atomic_store_relaxed(&From->Size, 0);
  }

  void enqueueBatch(QuarantineBatch *B) {
    List.push_back(B);
    addToSize(B->Size);
  }

  QuarantineBatch *dequeueBatch() {
    if (List.empty())
      return nullptr;
    QuarantineBatch *B = List.front();
    List.pop_front();
    subFromSize(B->Size);
    return B;
  }

  void mergeBatches(QuarantineCache *ToDeallocate) {
    uptr ExtractedSize = 0;
    QuarantineBatch *Current = List.front();
    while (Current && Current->Next) {
      if (Current->canMerge(Current->Next)) {
        QuarantineBatch *Extracted = Current->Next;
        // Move all the chunks into the current batch.
        Current->merge(Extracted);
        DCHECK_EQ(Extracted->Count, 0);
        DCHECK_EQ(Extracted->Size, sizeof(QuarantineBatch));
        // Remove the next batch From the list and account for its Size.
        List.extract(Current, Extracted);
        ExtractedSize += Extracted->Size;
        // Add it to deallocation list.
        ToDeallocate->enqueueBatch(Extracted);
      } else {
        Current = Current->Next;
      }
    }
    subFromSize(ExtractedSize);
  }

  AddrLimits addrLimits() const {
    AddrLimits Result;
    for (const QuarantineBatch &Batch : List)
      Result.combine(Batch.addrLimits());
    return Result;
  }

  void getStats(ScopedString *Str) const {
    uptr BatchCount = 0;
    uptr TotalOverheadBytes = 0;
    uptr TotalBytes = 0;
    uptr TotalQuarantineChunks = 0;
    for (const QuarantineBatch &Batch : List) {
      BatchCount++;
      TotalBytes += Batch.Size;
      TotalOverheadBytes += Batch.Size - Batch.getQuarantinedSize();
      TotalQuarantineChunks += Batch.Count;
    }
    const uptr QuarantineChunksCapacity =
        BatchCount * QuarantineBatch::MaxCount;
    const uptr ChunksUsagePercent =
        (QuarantineChunksCapacity == 0)
            ? 0
            : TotalQuarantineChunks * 100 / QuarantineChunksCapacity;
    const uptr TotalQuarantinedBytes = TotalBytes - TotalOverheadBytes;
    const uptr MemoryOverheadPercent =
        (TotalQuarantinedBytes == 0)
            ? 0
            : TotalOverheadBytes * 100 / TotalQuarantinedBytes;
    Str->append(
        "Stats: Quarantine: batches: %zu; bytes: %zu (user: %zu); chunks: %zu "
        "(capacity: %zu); %zu%% chunks used; %zu%% memory overhead\n",
        BatchCount, TotalBytes, TotalQuarantinedBytes, TotalQuarantineChunks,
        QuarantineChunksCapacity, ChunksUsagePercent, MemoryOverheadPercent);
  }

private:
  SinglyLinkedList<QuarantineBatch> List;
  atomic_uptr Size;

  void addToSize(uptr add) { atomic_store_relaxed(&Size, getSize() + add); }
  void subFromSize(uptr sub) { atomic_store_relaxed(&Size, getSize() - sub); }
};

template<typename QuarantineT>
void *sweeperThreadStart(void *QuarantinePtr) {
  QuarantineT *Quarantine = (QuarantineT*)QuarantinePtr;
  Quarantine->sweeperThreadMain();
  pthread_exit(NULL);
}

template <typename AllocatorT, typename Node> class GlobalQuarantine {
public:
  typedef QuarantineCache CacheT;
  typedef GlobalQuarantine<AllocatorT, Node> ThisT;
  typedef ShadowBitMap ShadowT;

  void initLinkerInitialized(uptr CacheSize) {
    atomic_store_relaxed(&MaxCacheSize, CacheSize);
    Cache.initLinkerInitialized();

    pthread_cond_init(&SweeperCondition, NULL);
    pthread_mutex_init(&SweeperMutex, NULL);
  }
  void init(AllocatorT *Allocator, uptr CacheSize) {
    this->Allocator = Allocator;
    CacheMutex.init();
    Cache.init();
    MaxCacheSize = {};
    initLinkerInitialized(CacheSize);
  }

  ~GlobalQuarantine() {
    killSweeperThread();
  }

  uptr getMaxSize() const {
    return SweepThreshold * Allocator->getTotalAllocatedUser() / 100;
  }
  uptr getCacheSize() const { return atomic_load_relaxed(&MaxCacheSize); }

  void put(CacheT *C, Node *Ptr, uptr Size) {
    C->enqueue(Ptr, Size);
    if (C->getSize() > getCacheSize())
      drain(C);
  }

  void NOINLINE drain(CacheT *C) {
    {
      ScopedLock L(CacheMutex);
      Cache.transfer(C);
    }
    if (Cache.getSize() > getMaxSize())
      recycle();
  }

  void NOINLINE drainAndRecycle(CacheT *C) {
    {
      ScopedLock L(CacheMutex);
      Cache.transfer(C);
    }
    recycle();
  }

  void getStats(ScopedString *Str) const {
    // It assumes that the world is stopped, just as the allocator's printStats.
    Cache.getStats(Str);
    Str->append("Quarantine limits: global: %zuK; thread local: %zuK\n",
                getMaxSize() >> 10, getCacheSize() >> 10);
  }

  void disable() {
    CacheMutex.lock();
  }

  void enable() {
    CacheMutex.unlock();
  }

  // Point of entry for SweeperThread
  void sweeperThreadMain() {
    ShadowMap.init(MIN_HEAP_ADDR, MAX_HEAP_ADDR - MIN_HEAP_ADDR, getPageSizeCached());

    // Repeat until program exit
    while (true) {
      // Wait until sweep is needed or program is shutting down
      bool SweepNeeded, ShutdownNeeded;
      pthread_mutex_lock(&SweeperMutex);
      while (true) {
        ShutdownNeeded = ShutdownSignal;
        SweepNeeded = Cache.getSize() > getMaxSize();
        if (ShutdownNeeded | SweepNeeded)
          break;
        pthread_cond_wait(&SweeperCondition, &SweeperMutex);
      }
      pthread_mutex_unlock(&SweeperMutex);

      if (ShutdownNeeded)
        return; // Kill thread

      DCHECK(SweepNeeded)
      doSweepAndRecycle();
    }
  }

private:
  // Read-only data.
  alignas(SCUDO_CACHE_LINE_SIZE) HybridMutex CacheMutex;
  CacheT Cache;
  AllocatorT *Allocator;
  atomic_uptr MaxSize;
  alignas(SCUDO_CACHE_LINE_SIZE) atomic_uptr MaxCacheSize;
  const uptr SweepThreshold = /* Sweep when */25/* % of all allocated memory is quarantined. */;
  // Sweeper thread
  pthread_t SweeperThread;
  volatile bool SweeperThreadLaunched;
  pthread_mutex_t SweeperMutex;
  pthread_cond_t SweeperCondition;
  volatile bool ShutdownSignal;
  ShadowT ShadowMap;

  void killSweeperThread() {
    pthread_mutex_lock(&SweeperMutex);
      ShutdownSignal = true;
    pthread_mutex_unlock(&SweeperMutex);
  }

  void recycle() {
    if (!pthread_mutex_trylock(&SweeperMutex)) {
      // Launch thread here. We use late initialisation for this to avoid deadlock in init()
      if (UNLIKELY(!SweeperThreadLaunched)) {
        SweeperThreadLaunched = true;
        pthread_create(&SweeperThread, nullptr, &sweeperThreadStart<ThisT>, this);
      }

      pthread_cond_signal(&SweeperCondition);
      pthread_mutex_unlock(&SweeperMutex);
    }
  }

  void NOINLINE doSweepAndRecycle() {
    CacheT ToCheck = gatherContents();
    
    AddrLimits PointerLimits = ToCheck.addrLimits();
    doSweepAndMark(PointerLimits);

    CacheT FailedFrees;
    FailedFrees.init();
    
    recycleUnmarked(FailedFrees, ToCheck);
    CacheT NewlyQuarantined = gatherContents();
    recycleUnmarked(FailedFrees, NewlyQuarantined, /*CheckRange=*/true, PointerLimits);

    Cache.transfer(&FailedFrees); // Reinsert failed frees
    ShadowMap.clear();
  }

  inline void doSweepAndMark(const AddrLimits& Limits) {
    auto MarkingLambda = [&](uptr Ptr, uptr Size) -> void {
      uptr* const Begin = (uptr*) Ptr;
      uptr* const End = (uptr*) (Ptr + Size);
      for (uptr *Word = Begin; Word < End; Word++) {
        uptr Target = *Word;
        // TODO(marton) better quick filtering: only mark if Target is in the plausible range
        if (!Limits.contains(Target))
            continue;
        ShadowMap.set(Target);
      }
    };

    Allocator->iterateOverActiveMemory(MarkingLambda);
  }

  inline CacheT gatherContents() {
    CacheT Result;
    Result.init();
    {
      ScopedLock L(CacheMutex);
      // Go over the batches and merge partially filled ones to
      // save some memory.
      const uptr CacheSize = Cache.getSize();
      const uptr OverheadSize = Cache.getOverheadSize();
      DCHECK_GE(CacheSize, OverheadSize);
      // Do the merge only when overhead exceeds this predefined limit (might
      // require some tuning). It saves us merge attempt when the batch list
      // quarantine is unlikely to contain batches suitable for merge.
      constexpr uptr OverheadThresholdPercents = 100;
      if (OverheadSize * (100 + OverheadThresholdPercents) >
              CacheSize * OverheadThresholdPercents) {
        Cache.mergeBatches(&Result);
      }

      // Remove batches from cache
      Result.transfer(&Cache);
    }

    return Result;
  }

  inline CacheT recycleUnmarked(CacheT &FailedFrees, CacheT &ToCheck, bool CheckRange = false,
                                AddrLimits Limits = AddrLimits()) {
    while (QuarantineBatch *B = ToCheck.dequeueBatch()) {
      constexpr uptr NumberOfPrefetch = 8UL;
      CHECK(NumberOfPrefetch <= ARRAY_SIZE(B->Ptrs));
      for (uptr I = 0; I < NumberOfPrefetch; I++) {
        PREFETCH(B->Ptrs[I]);
        PREFETCH(B->Sizes[I]);
      }
      for (uptr I = 0, Count = B->Count; I < Count; I++) {
        if (I + NumberOfPrefetch < Count) {
          PREFETCH(B->Ptrs[I + NumberOfPrefetch]);
          PREFETCH(B->Sizes[I + NumberOfPrefetch]);
        }

        void *Ptr = B->Ptrs[I];
        uptr Size = B->Sizes[I];
        uptr End = ((uptr)Ptr) + Size;

        // If something was inserted during the sweep, and it lies outside of the range
        // of pointers we considered, then fail to free it, and re-check on the next sweep
        bool OutOfRange = (CheckRange)
                              && (((uptr)Ptr < Limits.MinAddr) || (End >= Limits.MaxAddr));

        if ((OutOfRange) || marked((uptr)Ptr, Size)) {
          // Dirty => collect for reinsertion
          FailedFrees.enqueue(Ptr, Size);
        } else {
          // Clean => recycle
          Allocator->recycleChunk(reinterpret_cast<Node *>(B->Ptrs[I]));
        }
      }
      QuarantineBatch::deallocate(B);
    }

    return FailedFrees;
  }

  inline bool marked(uptr Ptr, uptr Size) {
    return !ShadowMap.allZero(Ptr, Ptr+Size);
  }
};

} // namespace scudo

#endif // SCUDO_QUARANTINE_H_
