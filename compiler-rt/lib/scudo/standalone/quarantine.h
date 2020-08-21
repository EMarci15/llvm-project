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
#include "secondary.h"
#include "string_utils.h"
#include "pool.h"
#include "pthread.h"
#include "tsd.h"

namespace scudo {

struct SavedSmallAlloc {
  uptr Ptr;
  uptr Size;

  uptr minAddress() const { return Ptr; }
  uptr maxAddress() const { return Ptr+Size; }
};

template<typename T, u32 MaxCount>
class QuarantineBatch {
public:
  using ThisT = QuarantineBatch<T,MaxCount>;
  using ItemT = T;
  QuarantineBatch *Next;
  uptr Size;
  u32 Count;
  T Items[MaxCount];

  static PoolAllocator<ThisT>* Pool() {
    static PoolAllocator<ThisT> P;
    return &P;
  }

  static ThisT *getNewInstance() {
    ThisT *Instance = Pool()->alloc();
    DCHECK(Instance);
    
    Instance->init();
    return Instance;
  }

  static void deallocate(ThisT *Instance) {
    Pool()->dealloc(Instance);
  }

  void init() {
    Count = 0;
    this->Size = sizeof(QuarantineBatch<T,MaxCount>); // Account for the Batch Size.
  }

  // The total size of quarantined nodes recorded in this batch.
  uptr getQuarantinedSize() const { return Size - sizeof(QuarantineBatch); }

  void push_back(const T& Item, uptr Size) {
    DCHECK_LT(Count, MaxCount);
    Items[Count++] = Item;
    this->Size += Size;
  }

  AddrLimits addrLimits() const {
    AddrLimits Result;
    for (uptr I = 0; I < Count; ++I)
      Result.combine(AddrLimits(Items[I].minAddress(), Items[I].maxAddress()));
    return Result;
  }

  bool canMerge(const QuarantineBatch<T,MaxCount> *const From) const {
    return Count + From->Count <= MaxCount;
  }

  void merge(QuarantineBatch<T,MaxCount> *const From) {
    DCHECK_LE(Count + From->Count, MaxCount);
    DCHECK_GE(Size, sizeof(QuarantineBatch));

    for (uptr I = 0; I < From->Count; ++I) {
      Items[Count + I] = From->Items[I];
    }
    Count += From->Count;
    Size += From->getQuarantinedSize();

    From->Count = 0;
    From->Size = sizeof(QuarantineBatch);
  }
};
  
template<typename T, u32 M>
typename QuarantineBatch<T,M>::template PoolAllocator<QuarantineBatch<T,M>> Pool;


const static u32 SmallBatchCount = 509;
const static u32 LargeBatchCount = 4064/sizeof(LargeBlock::SavedHeader); 

// Per-thread cache of memory blocks.
template<typename T, u32 MaxCount>
struct SpecificQuarantineCache {
  using BatchT = QuarantineBatch<T, MaxCount>;
  using ThisT = SpecificQuarantineCache<T, MaxCount>;

  void initLinkerInitialized() {}
  void init() {
    memset(this, 0, sizeof(*this));
    initLinkerInitialized();
  }

  // Total memory used, including internal accounting.
  uptr getSize() const { return atomic_load_relaxed(&Size); }
  // Memory used for internal accounting.
  uptr getOverheadSize() const { return List.size() * sizeof(BatchT); }

  void enqueue(const T& Item, uptr Size) {
    if (List.empty() || List.back()->Count == MaxCount) {
      BatchT *Batch = BatchT::getNewInstance();
      Batch->push_back(Item, Size);
      enqueueBatch(Batch);
    } else {
      List.back()->push_back(Item, Size);
      addToSize(Size);
    }
  }

  void transfer(SpecificQuarantineCache *From, bool merge) {
    if (merge && (From->List.size() == 1)) {
      BatchT *fromBatch = From->List.front();
      if ((!List.empty()) && (List.back()->canMerge(fromBatch))) {
        // Move over contents only
        List.back()->merge(fromBatch);
        return;
      }
    }

    List.append_back(&From->List);
    addToSize(From->getSize());
    atomic_store_relaxed(&From->Size, 0);
  }

  void enqueueBatch(BatchT *B) {
    List.push_back(B);
    addToSize(B->Size);
  }

  BatchT *dequeueBatch() {
    if (List.empty())
      return nullptr;
    BatchT *B = List.front();
    List.pop_front();
    subFromSize(B->Size);
    return B;
  }

  void mergeBatches() {
    uptr ExtractedSize = 0;
    BatchT *Current = List.front();
    while (Current && Current->Next) {
      if (Current->canMerge(Current->Next)) {
        BatchT *Extracted = Current->Next;
        // Move all the chunks into the current batch.
        Current->merge(Extracted);
        DCHECK_EQ(Extracted->Count, 0);
        DCHECK_EQ(Extracted->Size, sizeof(BatchT));
        // Remove the next batch From the list and account for its Size.
        List.extract(Current, Extracted);
        ExtractedSize += Extracted->Size;
        // Deallocate.
        BatchT::deallocate(Extracted);
      } else {
        Current = Current->Next;
      }
    }
    subFromSize(ExtractedSize);
  }

  void deallocateBatches() {
    while (!List.empty()) {
      BatchT *B = List.front();
      DCHECK(B->Count == 0);

      List.pop_front();
      BatchT::deallocate(B);
    }
  }

  AddrLimits addrLimits() const {
    AddrLimits Result;
    for (const BatchT &Batch : List)
      Result.combine(Batch.addrLimits());
    return Result;
  }

  void getStats(ScopedString *Str) const {
    uptr BatchCount = 0;
    uptr TotalOverheadBytes = 0;
    uptr TotalBytes = 0;
    uptr TotalQuarantineChunks = 0;
    for (const BatchT &Batch : List) {
      BatchCount++;
      TotalBytes += Batch.Size;
      TotalOverheadBytes += Batch.Size - Batch.getQuarantinedSize();
      TotalQuarantineChunks += Batch.Count;
    }
    const uptr QuarantineChunksCapacity = BatchCount * MaxCount;
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
  SinglyLinkedList<BatchT> List;
  atomic_uptr Size;

  void addToSize(uptr add) { atomic_store_relaxed(&Size, getSize() + add); }
  void subFromSize(uptr sub) { atomic_store_relaxed(&Size, getSize() - sub); }
};

struct QuarantineCache {
  using SmallCacheT = SpecificQuarantineCache<SavedSmallAlloc, SmallBatchCount>;
  using LargeCacheT = SpecificQuarantineCache<LargeBlock::SavedHeader, LargeBatchCount>;

  static constexpr bool IgnoreDecommitSize = true;

  SmallCacheT SmallCache;
  LargeCacheT LargeCache;
  using SmallBatchT = SmallCacheT::BatchT;
  using LargeBatchT = LargeCacheT::BatchT;

  void initLinkerInitialized() {}
  void init() {
    SmallCache.init();
    LargeCache.init();
    initLinkerInitialized();
  }

  // Total memory used, including internal accounting.
  uptr getSize() const {
    return SmallCache.getSize() + (1-IgnoreDecommitSize)*LargeCache.getSize();
  }
  // Memory used for internal accounting.
  uptr getOverheadSize() const
    { return SmallCache.getOverheadSize() + LargeCache.getOverheadSize();}

  inline void enqueueSmall(const SavedSmallAlloc& Item) {
    SmallCache.enqueue(Item, Item.Size);
  }
  inline void enqueueLarge(const LargeBlock::SavedHeader& Item) {
    LargeCache.enqueue(Item, Item.BlockEnd - Item.Block);
  }

  void transfer(QuarantineCache *From) {
    SmallCache.transfer(&From->SmallCache, 0);
    LargeCache.transfer(&From->LargeCache, 1);
  }

  inline void enqueueBatch(SmallBatchT *B) {
    SmallCache.enqueueBatch(B);
  }
  inline void enqueueBatch(LargeBatchT *B) {
    LargeCache.enqueueBatch(B);
  }

  void mergeBatches() {
    SmallCache.mergeBatches();
    LargeCache.mergeBatches();
  }

  void getStats(ScopedString *Str) const {
    SmallCache.getStats(Str);
    LargeCache.getStats(Str);
  }
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

  static constexpr uptr SweepThreshold
                          = /* Sweep when */25/* % of all allocated memory is in quarantine...*/;
  static constexpr uptr DecommitThreshold
                          = /* ...or decommitted memory size */300/* % of allocated memory. */;
  static constexpr bool IgnoreFailedFreeSize = true;
  static constexpr uptr SmallGranuleSizeLog = 5; // 32B

  void initLinkerInitialized(uptr CacheSize) {
    atomic_store_relaxed(&MaxCacheSize, CacheSize);
    Cache.initLinkerInitialized();

    pthread_cond_init(&SweeperCondition, NULL);
    pthread_cond_init(&BackStopCondition, NULL);
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

  inline uptr getMaxSize() const {
    return SweepThreshold * Allocator->getTotalAllocatedUser() / 100;
  }
  inline uptr getMaxDecommitSize() const {
    return DecommitThreshold * Allocator->getTotalAllocatedUser() / 100;
  }

  uptr getCacheSize() const { return atomic_load_relaxed(&MaxCacheSize); }

  void put(CacheT *C, const SavedSmallAlloc& Alloc,
           bool *UnlockRequired, TSD<AllocatorT>* TSD) {
    C->enqueueSmall(Alloc);
    if (C->getSize() > getCacheSize())
      drain(C, UnlockRequired, TSD);
  }
  void put(CacheT *C, const LargeBlock::SavedHeader& Header,
           bool *UnlockRequired, TSD<AllocatorT>* TSD) {
    C->enqueueLarge(Header);
    if (C->getSize() > getCacheSize())
      drain(C, UnlockRequired, TSD);
  }

  void NOINLINE drain(CacheT *C, bool *UnlockRequired, TSD<AllocatorT>* TSD) {
    {
      ScopedLock L(CacheMutex);
      Cache.transfer(C);
    }
    if (sweepNeeded())
      recycle(UnlockRequired, TSD);
  }

  void NOINLINE drainAndRecycle(CacheT *C) {
    {
      ScopedLock L(CacheMutex);
      Cache.transfer(C);
    }
    bool f = false;
    recycle(&f, NULL);
  }

  inline uptr effectiveSize() {
    uptr FailedFrees = IgnoreFailedFreeSize*atomic_load_relaxed(&FailedFreeSize);
    uptr CacheSize = Cache.getSize();
    if (CacheSize < FailedFrees)
      return 0; // FailedFree is out of date; do not wait/trigger sweep
    return CacheSize - FailedFrees;
  }

  inline bool sweepNeeded() {
    return (effectiveSize() > getMaxSize())
              || ((QuarantineCache::IgnoreDecommitSize)
                    && (Cache.LargeCache.getSize() > getMaxDecommitSize()));
  }

  inline bool backStopNeeded() {
    constexpr uptr MB = 1024*1024;
    constexpr uptr BackStopThreshold = 5;
    constexpr uptr BackStopLeeway = 100*MB;

    return effectiveSize() > BackStopThreshold*getMaxSize() + BackStopLeeway;
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
    uptr PageSizeLog = getLog2(getPageSizeCached());

    AddrLimits PrimaryLimits = Allocator->primaryLimits();
    SmallShadowMap.init(PrimaryLimits.MinAddr, PrimaryLimits.size(), SmallGranuleSizeLog);
    LargeShadowMap.init(MIN_HEAP_ADDR, MAX_HEAP_ADDR - MIN_HEAP_ADDR, PageSizeLog);

    // Repeat until program exit
    while (true) {
      // Wait until sweep is needed or program is shutting down
      bool SweepNeeded, ShutdownNeeded;
      pthread_mutex_lock(&SweeperMutex);
      pthread_cond_broadcast(&BackStopCondition);
      while (true) {
        ShutdownNeeded = ShutdownSignal;
        SweepNeeded = sweepNeeded();
        if (ShutdownNeeded | SweepNeeded)
          break;
        pthread_cond_wait(&SweeperCondition, &SweeperMutex);
      }
      pthread_mutex_unlock(&SweeperMutex);

      if (ShutdownNeeded)
        return; // Kill thread

      DCHECK(SweepNeeded);
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
  // Sweeper thread
  pthread_t SweeperThread;
  volatile bool SweeperThreadLaunched;
  pthread_mutex_t SweeperMutex;
  pthread_cond_t SweeperCondition;
  pthread_cond_t BackStopCondition;
  volatile bool ShutdownSignal;
  ShadowT SmallShadowMap, LargeShadowMap;
  atomic_uptr FailedFreeSize;

  void killSweeperThread() {
    pthread_mutex_lock(&SweeperMutex);
      ShutdownSignal = true;
    pthread_mutex_unlock(&SweeperMutex);
  }

  void recycle(bool* UnlockRequired, TSD<AllocatorT>* TSD) {
    if (LIKELY(TSD))
      Allocator->registerStack(TSD->StackRegistryIndex);

    if (!pthread_mutex_trylock(&SweeperMutex)) {
      // Launch thread here. We use late initialisation for this to avoid deadlock in init()
      if (UNLIKELY(!SweeperThreadLaunched)) {
        SweeperThreadLaunched = true;
        pthread_create(&SweeperThread, nullptr, &sweeperThreadStart<ThisT>, this);
      }

      pthread_cond_signal(&SweeperCondition);
      
      while (backStopNeeded()) {
        if (*UnlockRequired) {
          *UnlockRequired = false;
          TSD->unlock();
        }
        pthread_cond_wait(&BackStopCondition, &SweeperMutex);
      }

      pthread_mutex_unlock(&SweeperMutex);
    }
  }

  void NOINLINE doSweepAndRecycle() {
    CacheT ToCheck = gatherContents();
    atomic_store_relaxed(&FailedFreeSize, 0);
    
    AddrLimits SmallLimits = ToCheck.SmallCache.addrLimits();
    AddrLimits LargeLimits = ToCheck.LargeCache.addrLimits();
    doSweepAndMark(SmallLimits, LargeLimits);

    CacheT FailedFrees;
    FailedFrees.init();
    recycleUnmarked(FailedFrees, ToCheck);
    Allocator->postSweepCleanup();

    Cache.transfer(&FailedFrees); // Reinsert failed frees
    atomic_store_relaxed(&FailedFreeSize, FailedFrees.getSize());
    SmallShadowMap.clear();
    LargeShadowMap.clear();

    DCHECK(ToCheck.empty());
    FailedFrees.LargeCache.deallocateBatches();
  }

  inline void doSweepAndMark(const AddrLimits& SmallLimits, const AddrLimits& LargeLimits) {
    auto MarkingLambda = [&](uptr Ptr, uptr Size) -> void {
      uptr* const Begin = (uptr*) Ptr;
      uptr* const End = (uptr*) (Ptr + Size);
      for (uptr *Word = Begin; Word < End; Word++) {
        // Shift by header size to correctly associate end() pointers with the previous chunk
        uptr Target = (*Word) - Chunk::getHeaderSize();
        // Only mark if Target is in the plausible range
        if (SmallLimits.contains(Target)) {
          SmallShadowMap.set(Target);
        } else if (LargeLimits.contains(Target)) {
          LargeShadowMap.set(Target);
        }
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
        Cache.mergeBatches();
      }

      // Remove batches from cache
      Result.transfer(&Cache);
    }

    return Result;
  }

  template<class SpecificCacheT, typename SF, typename FF, typename MF>
  void checkUnmarked(SpecificCacheT &ToCheck, SF SuccessCb, FF FailureCb, MF marked) {
    while (typename SpecificCacheT::BatchT *B = ToCheck.dequeueBatch()) {
      constexpr uptr NumberOfPrefetch = 8UL;
      CHECK(NumberOfPrefetch <= ARRAY_SIZE(B->Items));
      for (uptr I = 0; I < NumberOfPrefetch; I++) PREFETCH(&B->Items[I]);
      for (uptr I = 0, Count = B->Count; I < Count; I++) {
        if (I + NumberOfPrefetch < Count) PREFETCH(&B->Items[I + NumberOfPrefetch]);

        auto& Item = B->Items[I];
        uptr Start = Item.minAddress();
        uptr End = Item.maxAddress();
        uptr Size = End - Start;
        CHECK(Start);

        if (marked(Start, Size)) {
          FailureCb(Item);
        } else {
          SuccessCb(Item);
        }
      }
      SpecificCacheT::BatchT::deallocate(B);
    }
  }

  inline void recycleUnmarked(CacheT &FailedFrees, CacheT &ToCheck) {
    auto SmallFailureCb =
      [&FailedFrees](const SavedSmallAlloc& Item) -> void { FailedFrees.enqueueSmall(Item); };
    auto SmallSuccessCb = [this](const SavedSmallAlloc& Item) -> void {
      Allocator->recycleChunk(Item);
    };
    auto LargeFailureCb = [&FailedFrees](const LargeBlock::SavedHeader& Item) -> void {
      FailedFrees.enqueueLarge(Item);
    };
    auto LargeSuccessCb = [this](const LargeBlock::SavedHeader& Item) -> void {
      Allocator->recycleChunk(Item);
    };
    auto SmallMarked = [this](uptr Ptr, uptr Size) -> bool {
      return !SmallShadowMap.allZero(Ptr, Ptr+Size);
    };
    auto LargeMarked = [this](uptr Ptr, uptr Size) -> bool {
      return !LargeShadowMap.allZero(Ptr, Ptr+Size);
    };

    checkUnmarked(ToCheck.SmallCache, SmallSuccessCb, SmallFailureCb, SmallMarked);
    checkUnmarked(ToCheck.LargeCache, LargeSuccessCb, LargeFailureCb, LargeMarked);
  }
};

} // namespace scudo

#endif // SCUDO_QUARANTINE_H_
